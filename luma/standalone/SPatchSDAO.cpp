// SPatch custom-AO coordinator for Sleeping Dogs: Definitive Edition.
//
// The replacement runs once at the game's exact final-lighting composition
// draw, where full-resolution depth and the native AO projection constants are
// available. Stock AO remains current as a strict same-frame fallback, but its
// texture is replaced at composition whenever the selected custom AO succeeds.

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
#include <cwchar>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SPatchGraphicsComponents.hpp"
#include "SPatchIni.hpp"
#include "SPatchReShadeCallbackSafety.hpp"
#include "SPatchSdaoDxbc.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kAoCoarseHash = 0xBC9D9B80;
constexpr std::uint32_t kAoFinalHash = 0x38B138FE;
constexpr std::uint32_t kAoBlurFirstHash = 0xF95B7E92;
constexpr std::uint32_t kAoBlurSecondHash = 0x2613FF5C;
constexpr std::uint32_t kFinalCompositionPixelShaderHash = 0x1964CD11;
constexpr std::array<std::uint32_t, 5> kStochasticLayerCounts = {1, 2, 2, 4, 4};

// Private-data tag placed only on the four stock AO compute shaders. D3D11
// keeps it with the shader object, avoiding a global pipeline map and a lock on
// every compute bind.
constexpr GUID kAoPassTag = {
    0xb8b88ed0,
    0x47b5,
    0x4f36,
    {0x94, 0x2d, 0x5c, 0x28, 0x0a, 0x8e, 0x8c, 0xf1},
};

constexpr GUID kPixelShaderHashTag = {
    0x560d2be3,
    0xe735,
    0x46b4,
    {0xb6, 0xab, 0x22, 0x78, 0xb5, 0x31, 0x75, 0x44},
};

constexpr GUID kInstrumentablePixelShaderTag = {
    0x7c84b89a,
    0xa1ea,
    0x4c37,
    {0xa5, 0x78, 0xa7, 0x9e, 0x12, 0x11, 0xbd, 0x71},
};

enum class AoPass : std::uint8_t {
    None = 0,
    Coarse,
    Final,
    BlurFirst,
    BlurSecond,
};

enum class AoMode : std::uint8_t {
    Original = 0,
    SDAO,
    GTAOLite,
};

struct Settings {
    AoMode mode = AoMode::Original;
    int quality = 2;
    float radius = 0.5f;
    float strength = 1.0f;
};

struct alignas(16) SdaoConstants {
    float radius;
    float strength;
    float falloff_range;
    float radius_multiplier;
};

static_assert(sizeof(SdaoConstants) == 16);

#if defined(SPATCH_SDAO_DEVELOPMENT)
struct GpuTimingSlot {
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> start;
    ComPtr<ID3D11Query> end;
    bool pending = false;
};
#endif

struct __declspec(uuid("1B058197-8897-4135-BA7B-7E109394EB24")) DeviceData {
    Settings settings;
    bool ready = false;
    ID3D11Device* native_device = nullptr;

    ComPtr<ID3D11ComputeShader> prepare_depth_shader;
    ComPtr<ID3D11ComputeShader> main_shader;
    ComPtr<ID3D11ComputeShader> horizontal_filter_shader;
    ComPtr<ID3D11ComputeShader> vertical_filter_shader;
    ComPtr<ID3D11SamplerState> point_sampler;
    ComPtr<ID3D11DepthStencilState> capture_depth_state;
    ComPtr<ID3D11BlendState> capture_min_blend_state;
    std::vector<std::uint8_t> capture_tail_bytecode;

    std::mutex instrumented_shader_mutex;
    std::unordered_map<std::uintptr_t, ComPtr<ID3D11PixelShader>>
        instrumented_pixel_shaders;
    std::atomic<std::uint64_t> instrumented_shader_count = 0;
    std::atomic<std::uint64_t> contiguous_fallback_shader_count = 0;
    std::atomic<std::uint64_t> rejected_shader_count = 0;

    ComPtr<ID3D11Texture2D> linear_depth;
    ComPtr<ID3D11UnorderedAccessView> linear_depth_uav;
    ComPtr<ID3D11ShaderResourceView> linear_depth_srv;

    ComPtr<ID3D11Texture2D> raw_ao;
    ComPtr<ID3D11UnorderedAccessView> raw_ao_uav;
    ComPtr<ID3D11ShaderResourceView> raw_ao_srv;
    ComPtr<ID3D11Texture2D> filtered_ao;
    ComPtr<ID3D11UnorderedAccessView> filtered_ao_uav;
    ComPtr<ID3D11ShaderResourceView> filtered_ao_srv;
    ComPtr<ID3D11Texture2D> final_ao;
    ComPtr<ID3D11UnorderedAccessView> final_ao_uav;
    ComPtr<ID3D11ShaderResourceView> final_ao_srv;
    ComPtr<ID3D11Texture2D> stochastic_depth;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> stochastic_depth_rtvs;
    ComPtr<ID3D11ShaderResourceView> stochastic_depth_srv;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Buffer> native_ao_constants;
    std::uint32_t native_ao_constants_size = 0;
    bool native_ao_constants_valid = false;
    std::uint64_t native_ao_constants_frame =
        (std::numeric_limits<std::uint64_t>::max)();

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool logged_success = false;
    bool logged_failure = false;
    std::uint32_t consecutive_composition_failures = 0;
    std::uint64_t replacement_frames = 0;
    std::uint32_t failed_width = 0;
    std::uint32_t failed_height = 0;
    std::uint32_t scratch_retry_cooldown = 0;
    bool logged_scratch_failure = false;
    std::uint32_t stochastic_width = 0;
    std::uint32_t stochastic_height = 0;
    std::uint32_t stochastic_layers = 0;
    std::uint32_t stochastic_pairs = 0;
    std::uint64_t capture_frame = (std::numeric_limits<std::uint64_t>::max)();
    std::uint32_t capture_draws = 0;
    std::uint32_t capture_misses = 0;
    bool capture_started = false;
    bool capture_valid = false;
    bool logged_capture_success = false;
    bool logged_capture_failure = false;
    std::uint64_t captured_frames = 0;
    std::array<ComPtr<ID3D11RenderTargetView>, 3>
        cached_camera_render_targets;
    ComPtr<ID3D11DepthStencilView> cached_camera_depth_stencil;
    std::uint32_t cached_camera_width = 0;
    std::uint32_t cached_camera_height = 0;
    bool camera_attachments_cached = false;
#if defined(SPATCH_SDAO_DEVELOPMENT)
    std::uint64_t camera_attachment_cache_hits = 0;
    std::uint64_t camera_attachment_cache_misses = 0;
    std::uint64_t camera_state_ticks_since_present = 0;
    std::uint64_t camera_state_calls_since_present = 0;
    std::array<double, 120> camera_state_frame_milliseconds{};
    std::size_t camera_state_timing_sample_count = 0;
    std::uint64_t camera_state_timing_sample_calls = 0;
    bool camera_state_timing_logged = false;
    std::array<GpuTimingSlot, 8> gpu_timing_slots{};
    std::array<double, 120> gpu_timing_samples{};
    std::size_t gpu_timing_cursor = 0;
    std::size_t gpu_timing_sample_count = 0;
    bool gpu_timing_ready = false;
    bool gpu_timing_logged = false;
    bool composition_bindings_logged = false;
    bool native_ao_bindings_logged = false;
#endif
};

#if defined(SPATCH_SDAO_DEVELOPMENT)
class CameraStateCpuTimer {
public:
    explicit CameraStateCpuTimer(DeviceData& data) noexcept : data_(data) {
        valid_ = QueryPerformanceCounter(&start_) != FALSE;
    }

    CameraStateCpuTimer(const CameraStateCpuTimer&) = delete;
    CameraStateCpuTimer& operator=(const CameraStateCpuTimer&) = delete;

    ~CameraStateCpuTimer() {
        LARGE_INTEGER end{};
        if (!valid_ || QueryPerformanceCounter(&end) == FALSE ||
            end.QuadPart < start_.QuadPart) {
            return;
        }
        data_.camera_state_ticks_since_present +=
            static_cast<std::uint64_t>(end.QuadPart - start_.QuadPart);
        ++data_.camera_state_calls_since_present;
    }

private:
    DeviceData& data_;
    LARGE_INTEGER start_{};
    bool valid_ = false;
};
#endif

struct __declspec(uuid("684A592B-D44A-4B92-93A3-E0169AE19D80")) CommandListData {
    AoPass current_pass = AoPass::None;
    std::uint32_t pixel_shader_hash = 0;
    bool pixel_shader_is_instrumentable = false;
};

HMODULE g_module = nullptr;
thread_local bool g_running_composition = false;
thread_local bool g_running_capture_draw = false;
thread_local bool g_creating_instrumented_shader = false;
std::atomic<std::uint64_t> g_present_count = 0;
#if defined(SPATCH_SDAO_DEVELOPMENT)
std::atomic<std::uint32_t> g_composition_draws_since_present = 0;
std::atomic<std::uint32_t> g_ao_coarse_dispatches_since_present = 0;
std::atomic<std::uint32_t> g_ao_final_dispatches_since_present = 0;
std::atomic<std::uint32_t> g_ao_blur_first_dispatches_since_present = 0;
std::atomic<std::uint32_t> g_ao_blur_second_dispatches_since_present = 0;
std::atomic<bool> g_development_enabled = true;
bool g_f5_was_down = false;
#endif

bool HasFreshNativeAoConstants(const DeviceData& data) noexcept {
    return data.native_ao_constants_valid && data.native_ao_constants &&
        data.native_ao_constants_frame ==
            g_present_count.load(std::memory_order_relaxed);
}

bool IsStandaloneRuntimeEnabled(const DeviceData& data) noexcept {
#if defined(SPATCH_SDAO_DEVELOPMENT)
    return data.settings.mode != AoMode::Original && data.ready &&
        g_development_enabled.load(std::memory_order_relaxed);
#else
    return data.settings.mode != AoMode::Original && data.ready;
#endif
}

bool IsSdaoSelected(const Settings& settings) noexcept {
    return settings.mode == AoMode::SDAO;
}

bool IsGtaoLiteSelected(const Settings& settings) noexcept {
    return settings.mode == AoMode::GTAOLite;
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
        "[ShenLong-SDAO] ReShade callback dropped after %s%s%s; native rendering remains active.",
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

AoMode ParseAoMode(
    const std::optional<std::wstring>& raw, AoMode fallback) noexcept {
    if (!raw) {
        return fallback;
    }
    const std::wstring& value = *raw;
    if (_wcsicmp(value.c_str(), L"SDAO") == 0 ||
        _wcsicmp(value.c_str(), L"GTAO") == 0 || value == L"1" ||
        _wcsicmp(value.c_str(), L"true") == 0 ||
        _wcsicmp(value.c_str(), L"on") == 0 ||
        _wcsicmp(value.c_str(), L"yes") == 0) {
        return AoMode::SDAO;
    }
    if (_wcsicmp(value.c_str(), L"Original") == 0 ||
        _wcsicmp(value.c_str(), L"Native") == 0 || value == L"0" ||
        _wcsicmp(value.c_str(), L"false") == 0 ||
        _wcsicmp(value.c_str(), L"off") == 0 ||
        _wcsicmp(value.c_str(), L"no") == 0) {
        return AoMode::Original;
    }
    if (_wcsicmp(value.c_str(), L"GTAOLite") == 0 ||
        _wcsicmp(value.c_str(), L"GTAO-Lite") == 0 ||
        _wcsicmp(value.c_str(), L"GTAO_Lite") == 0 || value == L"2") {
        return AoMode::GTAOLite;
    }
    return fallback;
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
        settings.mode = AoMode::Original;
        Log(reshade::log::level::error,
            "[ShenLong-SDAO] Could not resolve the add-on directory; using Original AO.");
        return settings;
    }

    const std::wstring path = directory + L"ShenLong.ini";
    namespace ini = spatch::graphics::ini;
    using ini::Key;
    constexpr std::array enabled_keys{
        Key{L"AmbientOcclusion", L"AmbientOcclusion"},
        Key{L"ShenLong", L"AmbientOcclusion"},
        Key{L"AmbientOcclusion", L"GTAO"},
        Key{L"ShenLong", L"GTAO"},
        Key{L"AmbientOcclusion", L"gtao_enable"},
        Key{L"ShenLong", L"gtao_enable"},
    };
    constexpr std::array sdao_quality_keys{
        Key{L"AmbientOcclusion", L"SDAOQuality"},
        Key{L"ShenLong", L"SDAOQuality"},
        Key{L"AmbientOcclusion", L"sdao_quality"},
        Key{L"ShenLong", L"sdao_quality"},
        Key{L"AmbientOcclusion", L"GTAOQuality"},
        Key{L"ShenLong", L"GTAOQuality"},
        Key{L"AmbientOcclusion", L"gtao_quality"},
        Key{L"ShenLong", L"gtao_quality"},
    };
    constexpr std::array sdao_radius_keys{
        Key{L"AmbientOcclusion", L"SDAORadius"},
        Key{L"ShenLong", L"SDAORadius"},
        Key{L"AmbientOcclusion", L"sdao_radius"},
        Key{L"ShenLong", L"sdao_radius"},
        Key{L"AmbientOcclusion", L"GTAORadius"},
        Key{L"ShenLong", L"GTAORadius"},
        Key{L"AmbientOcclusion", L"gtao_radius"},
        Key{L"ShenLong", L"gtao_radius"},
    };
    constexpr std::array sdao_strength_keys{
        Key{L"AmbientOcclusion", L"SDAOStrength"},
        Key{L"ShenLong", L"SDAOStrength"},
        Key{L"AmbientOcclusion", L"sdao_strength_percent"},
        Key{L"ShenLong", L"sdao_strength_percent"},
        Key{L"AmbientOcclusion", L"GTAOStrength"},
        Key{L"ShenLong", L"GTAOStrength"},
        Key{L"AmbientOcclusion", L"gtao_strength_percent"},
        Key{L"ShenLong", L"gtao_strength_percent"},
    };
    constexpr std::array gtao_lite_quality_keys{
        Key{L"AmbientOcclusion", L"GTAOLiteQuality"},
        Key{L"ShenLong", L"GTAOLiteQuality"},
        Key{L"AmbientOcclusion", L"gtao_lite_quality"},
        Key{L"ShenLong", L"gtao_lite_quality"},
    };
    constexpr std::array gtao_lite_radius_keys{
        Key{L"AmbientOcclusion", L"GTAOLiteRadius"},
        Key{L"ShenLong", L"GTAOLiteRadius"},
        Key{L"AmbientOcclusion", L"gtao_lite_radius"},
        Key{L"ShenLong", L"gtao_lite_radius"},
    };
    constexpr std::array gtao_lite_strength_keys{
        Key{L"AmbientOcclusion", L"GTAOLiteStrength"},
        Key{L"ShenLong", L"GTAOLiteStrength"},
        Key{L"AmbientOcclusion", L"gtao_lite_strength_percent"},
        Key{L"ShenLong", L"gtao_lite_strength_percent"},
    };

    const bool master_enabled = ini::ReadBool(path, ini::kMasterEnabledKeys, false);
    settings.mode = master_enabled
        ? ParseAoMode(ini::ReadFirst(path, enabled_keys), settings.mode)
        : AoMode::Original;
    const bool gtao_lite = IsGtaoLiteSelected(settings);
    if (gtao_lite) {
        settings.quality = ClampInt(ini::ReadInt(
            path, gtao_lite_quality_keys, settings.quality), 0, 4);
        settings.radius = ClampFloat(ini::ReadFloat(
            path, gtao_lite_radius_keys, settings.radius), 0.05f, 5.0f);
    } else {
        settings.quality = ClampInt(ini::ReadInt(
            path, sdao_quality_keys, settings.quality), 0, 4);
        settings.radius = ClampFloat(ini::ReadFloat(
            path, sdao_radius_keys, settings.radius), 0.05f, 5.0f);
    }
    const float strength_percent = ClampFloat(gtao_lite
        ? ini::ReadFloat(path, gtao_lite_strength_keys,
              settings.strength * 100.0f)
        : ini::ReadFloat(path, sdao_strength_keys,
              settings.strength * 100.0f),
        0.0f, 200.0f);
    settings.strength = strength_percent / 100.0f;

    Log(reshade::log::level::info,
        "[ShenLong-SDAO] config: mode=%s quality=%d radius=%.3fm strength=%.0f%%",
        settings.mode == AoMode::Original ? "Original" :
            (gtao_lite ? "GTAOLite" : "SDAO"),
        settings.quality,
        settings.radius,
        settings.strength * 100.0f);
    return settings;
}

bool NeedsAoInterception(
    const Settings& settings, reshade::api::device* device) noexcept {
#if defined(SPATCH_GRAPHICS_UNIFIED)
    return settings.mode != AoMode::Original ||
        spatch::graphics::gi::IsEnabled(device);
#else
    static_cast<void>(device);
    return settings.mode != AoMode::Original;
#endif
}

AoPass ClassifyShaderHash(std::uint32_t hash) noexcept {
    switch (hash) {
        case kAoCoarseHash:
            return AoPass::Coarse;
        case kAoFinalHash:
            return AoPass::Final;
        case kAoBlurFirstHash:
            return AoPass::BlurFirst;
        case kAoBlurSecondHash:
            return AoPass::BlurSecond;
        default:
            return AoPass::None;
    }
}

#if defined(SPATCH_SDAO_DEVELOPMENT)
bool CompileShaderBytecode(
    const std::wstring& shader_path,
    const char* entry_point,
    const char* profile,
    const D3D_SHADER_MACRO* macros,
    ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                           D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const HRESULT compile_result = D3DCompileFromFile(
        shader_path.c_str(),
        macros,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry_point,
        profile,
        flags,
        0,
        bytecode.GetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(compile_result) || !bytecode) {
        const char* detail = errors && errors->GetBufferPointer()
            ? static_cast<const char*>(errors->GetBufferPointer())
            : "no compiler diagnostics";
        Log(reshade::log::level::error,
            "[ShenLong-SDAO] Shader compile failed for %s: %s",
            entry_point,
            detail);
        return false;
    }
    return true;
}
#endif

bool LoadComputeShader(
    ID3D11Device* device,
    const std::wstring& cache_path,
    const std::wstring& shader_path,
    const char* entry_point,
    const D3D_SHADER_MACRO* macros,
    ComPtr<ID3D11ComputeShader>& shader,
    bool& used_source_fallback) {
    ComPtr<ID3DBlob> bytecode;
    const HRESULT load_result = D3DReadFileToBlob(
        cache_path.c_str(), bytecode.ReleaseAndGetAddressOf());
    if (FAILED(load_result) || !bytecode) {
#if defined(SPATCH_SDAO_DEVELOPMENT)
        used_source_fallback = true;
        Log(reshade::log::level::warning,
            "[ShenLong-SDAO] Precompiled shader cache unavailable for %s/cs_5_0 "
            "(HRESULT=0x%08X); using the Development source fallback.",
            entry_point,
            static_cast<unsigned int>(load_result));
        if (!CompileShaderBytecode(
                shader_path, entry_point, "cs_5_0", macros, bytecode)) {
            return false;
        }
#else
        static_cast<void>(shader_path);
        static_cast<void>(macros);
        static_cast<void>(used_source_fallback);
        Log(reshade::log::level::error,
            "[ShenLong-SDAO] Required precompiled shader cache entry is missing "
            "or invalid for %s/cs_5_0 (HRESULT=0x%08X).",
            entry_point,
            static_cast<unsigned int>(load_result));
        return false;
#endif
    }

    const HRESULT create_result = device->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        shader.ReleaseAndGetAddressOf());
    if (FAILED(create_result) || !shader) {
        Log(reshade::log::level::error,
            "[ShenLong-SDAO] CreateComputeShader failed for %s (HRESULT=0x%08X).",
            entry_point,
            static_cast<unsigned int>(create_result));
        return false;
    }
    return true;
}

bool LoadCaptureTailBytecode(
    const std::wstring& cache_path,
    const std::wstring& shader_path,
    const D3D_SHADER_MACRO* macros,
    std::vector<std::uint8_t>& bytecode,
    bool& used_source_fallback) {
    ComPtr<ID3DBlob> blob;
    const HRESULT load_result =
        D3DReadFileToBlob(cache_path.c_str(), blob.ReleaseAndGetAddressOf());
    if (FAILED(load_result) || !blob) {
#if defined(SPATCH_SDAO_DEVELOPMENT)
        used_source_fallback = true;
        Log(reshade::log::level::warning,
            "[ShenLong-SDAO] Precompiled shader cache unavailable for capture_depth_ps/ps_5_0 (HRESULT=0x%08X); using the Development source fallback.",
            static_cast<unsigned int>(load_result));
        if (!CompileShaderBytecode(
                shader_path, "capture_depth_ps", "ps_5_0", macros, blob)) {
            return false;
        }
#else
        static_cast<void>(shader_path);
        static_cast<void>(macros);
        static_cast<void>(used_source_fallback);
        Log(reshade::log::level::error,
            "[ShenLong-SDAO] Required precompiled shader cache entry is missing or invalid for capture_depth_ps/ps_5_0 (HRESULT=0x%08X).",
            static_cast<unsigned int>(load_result));
        return false;
#endif
    }
    const auto* first = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
    bytecode.assign(first, first + blob->GetBufferSize());
    return !bytecode.empty();
}

#if defined(SPATCH_SDAO_DEVELOPMENT)
void ResetGpuTiming(ID3D11Device* device, DeviceData& data) noexcept {
    data.gpu_timing_slots = {};
    data.gpu_timing_samples = {};
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
        if (FAILED(device->CreateQuery(
                &query_desc, slot.start.ReleaseAndGetAddressOf())) ||
            FAILED(device->CreateQuery(
                &query_desc, slot.end.ReleaseAndGetAddressOf()))) {
            data.gpu_timing_ready = false;
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
        directory + L"ShenLong\\Shaders\\SDAO\\SPatchSDAO.hlsl";
    const std::wstring cache_root =
        directory + L"ShenLong\\ShaderCache\\v1\\SDAO\\";

    static constexpr std::array<const char*, 5> quality_values = {
        "0", "1", "2", "3", "4"};
    const std::wstring quality_suffix =
        L".q" + std::to_wstring(data.settings.quality);
    const bool sdao = IsSdaoSelected(data.settings);
    const bool gtao_lite = IsGtaoLiteSelected(data.settings);
    const char* lite_value = gtao_lite ? "1" : "0";
    const D3D_SHADER_MACRO main_macros[] = {
        {"SD_SDAO_QUALITY", quality_values[data.settings.quality]},
        {"SD_GTAO_LITE", lite_value},
        {nullptr, nullptr},
    };
    const D3D_SHADER_MACRO horizontal_filter_macros[] = {
        {"SD_SDAO_QUALITY", quality_values[data.settings.quality]},
        {"SD_SDAO_FILTER_HORIZONTAL", "1"},
        {"SD_SDAO_COLOR_OUTPUT", "0"},
        {nullptr, nullptr},
    };
    const D3D_SHADER_MACRO vertical_filter_macros[] = {
        {"SD_SDAO_QUALITY", quality_values[data.settings.quality]},
        {"SD_SDAO_FILTER_HORIZONTAL", "0"},
        {"SD_SDAO_COLOR_OUTPUT", "1"},
        {nullptr, nullptr},
    };

    bool used_source_fallback = false;
    const std::uint32_t layer_count =
        kStochasticLayerCounts[static_cast<std::size_t>(data.settings.quality)];
    const std::wstring capture_suffix = quality_suffix + L".layers" +
        std::to_wstring(layer_count) + L".full1.cso";
    if ((sdao && !LoadCaptureTailBytecode(
             cache_root + L"capture_depth_ps.ps_5_0" + capture_suffix,
             shader_path,
             main_macros,
             data.capture_tail_bytecode,
             used_source_fallback)) ||
        !LoadComputeShader(
            device,
            cache_root + L"prepare_depth_cs.cs_5_0.cso",
            shader_path,
            "prepare_depth_cs",
            nullptr,
            data.prepare_depth_shader,
            used_source_fallback) ||
        !LoadComputeShader(
            device,
            cache_root + L"main_pass_cs.cs_5_0" + quality_suffix +
                (gtao_lite ? L".gtaolite1.cso" : L".cso"),
            shader_path, "main_pass_cs", main_macros, data.main_shader,
            used_source_fallback) ||
        !LoadComputeShader(
            device,
            cache_root + L"spatial_filter_cs.cs_5_0" + quality_suffix +
                L".horizontal1.color0.cso",
            shader_path,
            "spatial_filter_cs",
            horizontal_filter_macros,
            data.horizontal_filter_shader,
            used_source_fallback) ||
        !LoadComputeShader(
            device,
            cache_root + L"spatial_filter_cs.cs_5_0" + quality_suffix +
                L".horizontal0.color1.cso",
            shader_path,
            "spatial_filter_cs",
            vertical_filter_macros,
            data.vertical_filter_shader,
            used_source_fallback)) {
        return false;
    }
    if (used_source_fallback) {
        Log(reshade::log::level::info,
            "[ShenLong-SDAO] Shader bytecode initialized with the Development "
            "source fallback.");
    } else {
        Log(reshade::log::level::info,
            "[ShenLong-SDAO] Precompiled shader cache v1 loaded (mode=%s quality=%d).",
            gtao_lite ? "GTAOLite" : "SDAO",
            data.settings.quality);
    }

    const D3D11_SAMPLER_DESC sampler_desc = {
        D3D11_FILTER_MIN_MAG_MIP_POINT,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        0.0f,
        1,
        D3D11_COMPARISON_NEVER,
        {0.0f, 0.0f, 0.0f, 0.0f},
        0.0f,
        D3D11_FLOAT32_MAX,
    };
    const HRESULT sampler_result =
        device->CreateSamplerState(&sampler_desc, data.point_sampler.ReleaseAndGetAddressOf());
    if (FAILED(sampler_result) || !data.point_sampler) {
        Log(reshade::log::level::error,
            "[ShenLong-SDAO] CreateSamplerState failed (HRESULT=0x%08X).",
            static_cast<unsigned int>(sampler_result));
        return false;
    }

    if (sdao) {
        UINT capture_float_support = 0;
        UINT capture_uint_support = 0;
        constexpr UINT required_capture_float_support =
            D3D11_FORMAT_SUPPORT_TEXTURE2D |
            D3D11_FORMAT_SUPPORT_RENDER_TARGET |
            D3D11_FORMAT_SUPPORT_BLENDABLE;
        constexpr UINT required_capture_uint_support =
            D3D11_FORMAT_SUPPORT_TEXTURE2D |
            D3D11_FORMAT_SUPPORT_SHADER_LOAD;
        if (FAILED(device->CheckFormatSupport(
                DXGI_FORMAT_R32G32_FLOAT, &capture_float_support)) ||
            (capture_float_support & required_capture_float_support) !=
                required_capture_float_support ||
            FAILED(device->CheckFormatSupport(
                DXGI_FORMAT_R32G32_UINT, &capture_uint_support)) ||
            (capture_uint_support & required_capture_uint_support) !=
                required_capture_uint_support) {
            Log(reshade::log::level::error,
                "[ShenLong-SDAO] R32G32 MIN-blend capture is unsupported (float-support=0x%08X uint-support=0x%08X).",
                capture_float_support, capture_uint_support);
            return false;
        }

        // Capture uses no DSV. Native alpha discard runs in the sliced shader,
        // and fixed-function MIN blending retains the nearest selected depth
        // without a UAV atomic or any native MRT/depth write.
        D3D11_DEPTH_STENCIL_DESC capture_depth_desc{};
        capture_depth_desc.DepthEnable = FALSE;
        capture_depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        capture_depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        const HRESULT capture_depth_result = device->CreateDepthStencilState(
            &capture_depth_desc,
            data.capture_depth_state.ReleaseAndGetAddressOf());
        if (FAILED(capture_depth_result) || !data.capture_depth_state) {
            Log(reshade::log::level::error,
                "[ShenLong-SDAO] CreateDepthStencilState failed for stochastic capture (HRESULT=0x%08X).",
                static_cast<unsigned int>(capture_depth_result));
            return false;
        }

        D3D11_BLEND_DESC capture_blend_desc{};
        capture_blend_desc.IndependentBlendEnable = FALSE;
        D3D11_RENDER_TARGET_BLEND_DESC& capture_blend =
            capture_blend_desc.RenderTarget[0];
        capture_blend.BlendEnable = TRUE;
        capture_blend.SrcBlend = D3D11_BLEND_ONE;
        capture_blend.DestBlend = D3D11_BLEND_ONE;
        capture_blend.BlendOp = D3D11_BLEND_OP_MIN;
        capture_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        capture_blend.DestBlendAlpha = D3D11_BLEND_ONE;
        capture_blend.BlendOpAlpha = D3D11_BLEND_OP_MIN;
        capture_blend.RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
        const HRESULT capture_blend_result = device->CreateBlendState(
            &capture_blend_desc,
            data.capture_min_blend_state.ReleaseAndGetAddressOf());
        if (FAILED(capture_blend_result) ||
            !data.capture_min_blend_state) {
            Log(reshade::log::level::error,
                "[ShenLong-SDAO] CreateBlendState failed for stochastic MIN capture (HRESULT=0x%08X).",
                static_cast<unsigned int>(capture_blend_result));
            return false;
        }
    }
#if defined(SPATCH_SDAO_DEVELOPMENT)
    ResetGpuTiming(device, data);
#endif
    return true;
}

void ResetScratch(DeviceData& data) noexcept {
    data.linear_depth.Reset();
    data.linear_depth_uav.Reset();
    data.linear_depth_srv.Reset();
    data.raw_ao.Reset();
    data.raw_ao_uav.Reset();
    data.raw_ao_srv.Reset();
    data.filtered_ao.Reset();
    data.filtered_ao_uav.Reset();
    data.filtered_ao_srv.Reset();
    data.final_ao.Reset();
    data.final_ao_uav.Reset();
    data.final_ao_srv.Reset();
    data.constants.Reset();
    data.width = 0;
    data.height = 0;
}

void ResetStochasticDepth(DeviceData& data) noexcept {
    data.stochastic_depth.Reset();
    for (auto& rtv : data.stochastic_depth_rtvs) {
        rtv.Reset();
    }
    data.stochastic_depth_srv.Reset();
    data.stochastic_width = 0;
    data.stochastic_height = 0;
    data.stochastic_layers = 0;
    data.stochastic_pairs = 0;
    data.capture_started = false;
    data.capture_valid = false;
    data.capture_draws = 0;
    data.capture_misses = 0;
}

bool CreateStochasticDepth(
    ID3D11Device* device,
    DeviceData& data,
    std::uint32_t width,
    std::uint32_t height) {
    const std::uint32_t layers =
        kStochasticLayerCounts[static_cast<std::size_t>(data.settings.quality)];
    const std::uint32_t pairs = (layers + 1u) / 2u;
    const bool rtvs_ready = std::all_of(
        data.stochastic_depth_rtvs.begin(),
        data.stochastic_depth_rtvs.begin() + pairs,
        [](const ComPtr<ID3D11RenderTargetView>& rtv) {
            return rtv.Get() != nullptr;
        });
    if (data.stochastic_depth && rtvs_ready &&
        data.stochastic_depth_srv && data.stochastic_width == width &&
        data.stochastic_height == height && data.stochastic_layers == layers &&
        data.stochastic_pairs == pairs) {
        return true;
    }
    if (!device || width < 16 || height < 16 || layers == 0 ||
        pairs == 0 || pairs > data.stochastic_depth_rtvs.size()) {
        return false;
    }

    ComPtr<ID3D11Texture2D> texture;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> rtvs;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = pairs;
    texture_desc.Format = DXGI_FORMAT_R32G32_TYPELESS;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(
            &texture_desc, nullptr, texture.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv_desc.Texture2DArray.MipSlice = 0;
    rtv_desc.Texture2DArray.ArraySize = 1;
    for (std::uint32_t pair = 0; pair < pairs; ++pair) {
        rtv_desc.Texture2DArray.FirstArraySlice = pair;
        if (FAILED(device->CreateRenderTargetView(
                texture.Get(), &rtv_desc, rtvs[pair].ReleaseAndGetAddressOf()))) {
            return false;
        }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R32G32_UINT;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray.MostDetailedMip = 0;
    srv_desc.Texture2DArray.MipLevels = 1;
    srv_desc.Texture2DArray.FirstArraySlice = 0;
    srv_desc.Texture2DArray.ArraySize = pairs;
    if (FAILED(device->CreateShaderResourceView(
            texture.Get(), &srv_desc, srv.ReleaseAndGetAddressOf()))) {
        return false;
    }

    ResetStochasticDepth(data);
    data.stochastic_depth = std::move(texture);
    data.stochastic_depth_rtvs = std::move(rtvs);
    data.stochastic_depth_srv = std::move(srv);
    data.stochastic_width = width;
    data.stochastic_height = height;
    data.stochastic_layers = layers;
    data.stochastic_pairs = pairs;
    return true;
}

bool CreateScratch(
    ID3D11Device* device, DeviceData& data, std::uint32_t width, std::uint32_t height) {
    if (data.width == width && data.height == height && data.linear_depth &&
        data.linear_depth_uav && data.linear_depth_srv && data.raw_ao &&
        data.raw_ao_uav && data.raw_ao_srv && data.filtered_ao &&
        data.filtered_ao_uav && data.filtered_ao_srv && data.final_ao &&
        data.final_ao_uav && data.final_ao_srv && data.constants) {
        return true;
    }

    if (width < 16 || height < 16) {
        return false;
    }

    const bool repeated_failure =
        data.failed_width == width && data.failed_height == height;
    if (repeated_failure && data.scratch_retry_cooldown != 0) {
        --data.scratch_retry_cooldown;
        return false;
    }
    if (!repeated_failure) {
        data.logged_scratch_failure = false;
    }
    const auto record_failure = [&data, width, height]() noexcept {
        data.failed_width = width;
        data.failed_height = height;
        data.scratch_retry_cooldown = 120;
        if (!data.logged_scratch_failure) {
            data.logged_scratch_failure = true;
            Log(reshade::log::level::warning,
                "[ShenLong-SDAO] scratch allocation failed at %ux%u; native AO remains active and retry is rate-limited.",
                width,
                height);
        }
        return false;
    };

    ComPtr<ID3D11Texture2D> linear_depth;
    ComPtr<ID3D11UnorderedAccessView> linear_depth_uav;
    ComPtr<ID3D11ShaderResourceView> linear_depth_srv;
    ComPtr<ID3D11Texture2D> raw_ao;
    ComPtr<ID3D11UnorderedAccessView> raw_ao_uav;
    ComPtr<ID3D11ShaderResourceView> raw_ao_srv;
    ComPtr<ID3D11Texture2D> filtered_ao;
    ComPtr<ID3D11UnorderedAccessView> filtered_ao_uav;
    ComPtr<ID3D11ShaderResourceView> filtered_ao_srv;
    ComPtr<ID3D11Texture2D> final_ao;
    ComPtr<ID3D11UnorderedAccessView> final_ao_uav;
    ComPtr<ID3D11ShaderResourceView> final_ao_srv;
    ComPtr<ID3D11Buffer> constants_buffer;

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R32_FLOAT;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(
            &texture_desc, nullptr, linear_depth.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateUnorderedAccessView(
            linear_depth.Get(),
            nullptr,
            linear_depth_uav.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(
            linear_depth.Get(),
            nullptr,
            linear_depth_srv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }

    texture_desc.Format = DXGI_FORMAT_R16_FLOAT;
    const auto create_float_target = [device, &texture_desc](
        ComPtr<ID3D11Texture2D>& texture,
        ComPtr<ID3D11UnorderedAccessView>& uav,
        ComPtr<ID3D11ShaderResourceView>& srv) {
        return SUCCEEDED(device->CreateTexture2D(
                   &texture_desc, nullptr, texture.ReleaseAndGetAddressOf())) &&
               SUCCEEDED(device->CreateUnorderedAccessView(
                   texture.Get(), nullptr, uav.ReleaseAndGetAddressOf())) &&
               SUCCEEDED(device->CreateShaderResourceView(
                   texture.Get(), nullptr, srv.ReleaseAndGetAddressOf()));
    };
    if (!create_float_target(raw_ao, raw_ao_uav, raw_ao_srv) ||
        !create_float_target(
            filtered_ao, filtered_ao_uav, filtered_ao_srv)) {
        return record_failure();
    }

    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device->CreateTexture2D(
            &texture_desc, nullptr, final_ao.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateUnorderedAccessView(
            final_ao.Get(), nullptr, final_ao_uav.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(
            final_ao.Get(), nullptr, final_ao_srv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }

    const SdaoConstants constants = {
        data.settings.radius,
        data.settings.strength,
        0.615f,
        1.457f,
    };
    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(constants);
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial_data{};
    initial_data.pSysMem = &constants;
    if (FAILED(device->CreateBuffer(
            &buffer_desc, &initial_data, constants_buffer.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }

    ResetScratch(data);
    data.linear_depth = std::move(linear_depth);
    data.linear_depth_uav = std::move(linear_depth_uav);
    data.linear_depth_srv = std::move(linear_depth_srv);
    data.raw_ao = std::move(raw_ao);
    data.raw_ao_uav = std::move(raw_ao_uav);
    data.raw_ao_srv = std::move(raw_ao_srv);
    data.filtered_ao = std::move(filtered_ao);
    data.filtered_ao_uav = std::move(filtered_ao_uav);
    data.filtered_ao_srv = std::move(filtered_ao_srv);
    data.final_ao = std::move(final_ao);
    data.final_ao_uav = std::move(final_ao_uav);
    data.final_ao_srv = std::move(final_ao_srv);
    data.constants = std::move(constants_buffer);
    data.width = width;
    data.height = height;
    data.failed_width = 0;
    data.failed_height = 0;
    data.scratch_retry_cooldown = 0;
    data.logged_scratch_failure = false;
#if defined(SPATCH_SDAO_DEVELOPMENT)
    ResetGpuTiming(device, data);
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
    explicit ScopedComputeState(ID3D11DeviceContext* context) noexcept : context_(context) {
        std::array<ID3D11ClassInstance*, 256> class_instances{};
        class_instance_count_ = static_cast<UINT>(class_instances.size());
        context_->CSGetShader(
            shader_.GetAddressOf(), class_instances.data(), &class_instance_count_);
        for (UINT index = 0; index < class_instance_count_; ++index) {
            class_instances_[index].Attach(class_instances[index]);
        }

        context_->CSGetConstantBuffers(9, 1, constant_buffer_9_.GetAddressOf());
        context_->CSGetConstantBuffers(11, 1, constant_buffer_11_.GetAddressOf());
        context_->CSGetSamplers(0, 1, sampler_.GetAddressOf());

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        context_->CSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        AttachReturnedInterfaces(srvs_, srvs.data());

        context_->CSGetUnorderedAccessViews(0, 1, uav_.GetAddressOf());
        context_->GetPredication(predicate_.GetAddressOf(), &predicate_value_);
        context_->SetPredication(nullptr, FALSE);
    }

    ScopedComputeState(const ScopedComputeState&) = delete;
    ScopedComputeState& operator=(const ScopedComputeState&) = delete;

    ~ScopedComputeState() noexcept {
        static constexpr std::array<ID3D11ShaderResourceView*, 3> null_srvs{};
        ID3D11UnorderedAccessView* null_uav = nullptr;
        context_->CSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context_->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);

        ID3D11UnorderedAccessView* uav = uav_.Get();
        const UINT keep_counter = D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
        context_->CSSetUnorderedAccessViews(0, 1, &uav, &keep_counter);

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        for (std::size_t index = 0; index < srvs.size(); ++index) {
            srvs[index] = srvs_[index].Get();
        }
        context_->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());

        ID3D11Buffer* constant_buffer = constant_buffer_9_.Get();
        context_->CSSetConstantBuffers(9, 1, &constant_buffer);
        constant_buffer = constant_buffer_11_.Get();
        context_->CSSetConstantBuffers(11, 1, &constant_buffer);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->CSSetSamplers(0, 1, &sampler);

        std::array<ID3D11ClassInstance*, 256> class_instances{};
        for (UINT index = 0; index < class_instance_count_; ++index) {
            class_instances[index] = class_instances_[index].Get();
        }
        context_->CSSetShader(shader_.Get(), class_instances.data(), class_instance_count_);
        context_->SetPredication(predicate_.Get(), predicate_value_);
    }

private:
    ID3D11DeviceContext* context_;
    ComPtr<ID3D11ComputeShader> shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> class_instances_;
    UINT class_instance_count_ = 0;
    ComPtr<ID3D11Buffer> constant_buffer_9_;
    ComPtr<ID3D11Buffer> constant_buffer_11_;
    ComPtr<ID3D11SamplerState> sampler_;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> srvs_;
    ComPtr<ID3D11UnorderedAccessView> uav_;
    ComPtr<ID3D11Predicate> predicate_;
    BOOL predicate_value_ = FALSE;
};

bool GetTextureDescription(
    ID3D11ShaderResourceView* view, D3D11_TEXTURE2D_DESC& description) {
    if (!view) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
    view->GetDesc(&view_description);
    if (view_description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        view_description.Texture2D.MostDetailedMip != 0) {
        return false;
    }

    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    view->GetResource(resource.GetAddressOf());
    if (!resource || FAILED(resource.As(&texture))) {
        return false;
    }
    texture->GetDesc(&description);
    return true;
}

bool MatchesCameraRenderTarget(
    ID3D11RenderTargetView* view,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    if (!view) {
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    view->GetResource(resource.GetAddressOf());
    if (!resource || FAILED(resource.As(&texture)) || !texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return description.Width == width && description.Height == height &&
        description.MipLevels == 1 && description.ArraySize == 1 &&
        description.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.SampleDesc.Count == 1;
}

bool MatchesCameraDepthStencil(
    ID3D11DepthStencilView* view,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    if (!view) {
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    view->GetResource(resource.GetAddressOf());
    if (!resource || FAILED(resource.As(&texture)) || !texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return description.Width == width && description.Height == height &&
        description.MipLevels == 1 && description.ArraySize == 1 &&
        description.Format == DXGI_FORMAT_R24G8_TYPELESS &&
        description.SampleDesc.Count == 1;
}

struct CameraDrawState {
    std::array<ComPtr<ID3D11RenderTargetView>, 3> render_targets;
    ComPtr<ID3D11DepthStencilView> depth_stencil;
    ComPtr<ID3D11DepthStencilState> depth_state;
    UINT stencil_reference = 0;
    ComPtr<ID3D11BlendState> blend_state;
    std::array<FLOAT, 4> blend_factor{};
    UINT sample_mask = D3D11_DEFAULT_SAMPLE_MASK;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 5> old_uavs;
    ComPtr<ID3D11PixelShader> original_pixel_shader;
    ComPtr<ID3D11PixelShader> instrumented_pixel_shader;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool uav_conflict = false;
};

bool ReadCameraDrawState(
    ID3D11DeviceContext* context,
    DeviceData& data,
    CameraDrawState& state,
    bool& required_depth_draw) {
    required_depth_draw = false;
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }
#if defined(SPATCH_SDAO_DEVELOPMENT)
    CameraStateCpuTimer cpu_timer(data);
#endif
    std::array<ID3D11RenderTargetView*, 8> all_render_target_raw{};
    ComPtr<ID3D11DepthStencilView> depth_stencil;
    context->OMGetRenderTargets(
        static_cast<UINT>(all_render_target_raw.size()),
        all_render_target_raw.data(),
        depth_stencil.GetAddressOf());
    std::array<ComPtr<ID3D11RenderTargetView>, 8> all_render_targets;
    AttachReturnedInterfaces(all_render_targets, all_render_target_raw.data());
    if (!all_render_targets[0] || !all_render_targets[1] ||
        !all_render_targets[2] || all_render_targets[3] ||
        all_render_targets[4] || all_render_targets[5] ||
        all_render_targets[6] || all_render_targets[7] || !depth_stencil) {
        return false;
    }

    const bool attachments_cached = data.camera_attachments_cached &&
        std::equal(
            data.cached_camera_render_targets.begin(),
            data.cached_camera_render_targets.end(),
            all_render_targets.begin(),
            [](const ComPtr<ID3D11RenderTargetView>& cached,
               const ComPtr<ID3D11RenderTargetView>& current) {
                return cached.Get() == current.Get();
            }) &&
        data.cached_camera_depth_stencil.Get() == depth_stencil.Get();
    if (attachments_cached) {
        state.width = data.cached_camera_width;
        state.height = data.cached_camera_height;
#if defined(SPATCH_SDAO_DEVELOPMENT)
        ++data.camera_attachment_cache_hits;
#endif
    } else {
#if defined(SPATCH_SDAO_DEVELOPMENT)
        ++data.camera_attachment_cache_misses;
#endif
        ComPtr<ID3D11Resource> first_resource;
        ComPtr<ID3D11Texture2D> first_texture;
        all_render_targets[0]->GetResource(first_resource.GetAddressOf());
        if (!first_resource || FAILED(first_resource.As(&first_texture)) ||
            !first_texture) {
            return false;
        }
        D3D11_TEXTURE2D_DESC first_description{};
        first_texture->GetDesc(&first_description);
        if (first_description.Width < 16 || first_description.Height < 16 ||
            !MatchesCameraRenderTarget(
                all_render_targets[0].Get(),
                first_description.Width,
                first_description.Height) ||
            !MatchesCameraRenderTarget(
                all_render_targets[1].Get(),
                first_description.Width,
                first_description.Height) ||
            !MatchesCameraRenderTarget(
                all_render_targets[2].Get(),
                first_description.Width,
                first_description.Height) ||
            !MatchesCameraDepthStencil(
                depth_stencil.Get(),
                first_description.Width,
                first_description.Height)) {
            return false;
        }
        for (std::size_t index = 0;
             index < data.cached_camera_render_targets.size();
             ++index) {
            data.cached_camera_render_targets[index] = all_render_targets[index];
        }
        data.cached_camera_depth_stencil = depth_stencil;
        data.cached_camera_width = first_description.Width;
        data.cached_camera_height = first_description.Height;
        data.camera_attachments_cached = true;
        state.width = first_description.Width;
        state.height = first_description.Height;
    }

    std::array<ID3D11UnorderedAccessView*, 5> uav_raw{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(
        0,
        nullptr,
        nullptr,
        3,
        static_cast<UINT>(uav_raw.size()),
        uav_raw.data());
    AttachReturnedInterfaces(state.old_uavs, uav_raw.data());
    state.uav_conflict = std::any_of(
        state.old_uavs.begin(), state.old_uavs.end(),
        [](const ComPtr<ID3D11UnorderedAccessView>& view) {
            return view.Get() != nullptr;
        });

    ComPtr<ID3D11DepthStencilState> depth_state;
    UINT stencil_reference = 0;
    context->OMGetDepthStencilState(
        depth_state.GetAddressOf(), &stencil_reference);
    if (!depth_state) {
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth_description{};
    depth_state->GetDesc(&depth_description);
    if (!depth_description.DepthEnable ||
        depth_description.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL ||
        depth_description.DepthFunc != D3D11_COMPARISON_LESS_EQUAL ||
        depth_description.StencilEnable) {
        return false;
    }
    required_depth_draw = true;

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        return false;
    }

    ComPtr<ID3D11BlendState> blend_state;
    std::array<FLOAT, 4> blend_factor{};
    UINT sample_mask = 0;
    context->OMGetBlendState(
        blend_state.GetAddressOf(), blend_factor.data(), &sample_mask);
    if (blend_state) {
        D3D11_BLEND_DESC blend_description{};
        blend_state->GetDesc(&blend_description);
        if (blend_description.AlphaToCoverageEnable ||
            blend_description.IndependentBlendEnable ||
            blend_description.RenderTarget[0].BlendEnable ||
            blend_description.RenderTarget[1].BlendEnable ||
            blend_description.RenderTarget[2].BlendEnable) {
            return false;
        }
    }

    ComPtr<ID3D11RasterizerState> rasterizer_state;
    context->RSGetState(rasterizer_state.GetAddressOf());
    if (rasterizer_state) {
        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_state->GetDesc(&rasterizer_description);
        if (rasterizer_description.FillMode != D3D11_FILL_SOLID ||
            (rasterizer_description.CullMode != D3D11_CULL_NONE &&
             rasterizer_description.CullMode != D3D11_CULL_BACK) ||
            !rasterizer_description.DepthClipEnable) {
            return false;
        }
    }

    std::array<D3D11_VIEWPORT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    UINT viewport_count = static_cast<UINT>(viewports.size());
    context->RSGetViewports(&viewport_count, viewports.data());
    if (viewport_count != 1 || viewports[0].TopLeftX != 0.0f ||
        viewports[0].TopLeftY != 0.0f ||
        viewports[0].Width != static_cast<float>(state.width) ||
        viewports[0].Height != static_cast<float>(state.height) ||
        viewports[0].MinDepth != 0.0f || viewports[0].MaxDepth != 1.0f) {
        return false;
    }

    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11HullShader> hull_shader;
    ComPtr<ID3D11DomainShader> domain_shader;
    ComPtr<ID3D11GeometryShader> geometry_shader;
    context->VSGetShader(vertex_shader.GetAddressOf(), nullptr, nullptr);
    context->HSGetShader(hull_shader.GetAddressOf(), nullptr, nullptr);
    context->DSGetShader(domain_shader.GetAddressOf(), nullptr, nullptr);
    context->GSGetShader(geometry_shader.GetAddressOf(), nullptr, nullptr);
    if (!vertex_shader || hull_shader || domain_shader || geometry_shader) {
        return false;
    }

    std::array<ID3D11ClassInstance*, 256> class_instances{};
    UINT class_instance_count = static_cast<UINT>(class_instances.size());
    ID3D11PixelShader* original_pixel_shader = nullptr;
    context->PSGetShader(
        &original_pixel_shader,
        class_instances.data(),
        &class_instance_count);
    state.original_pixel_shader.Attach(original_pixel_shader);
    for (UINT index = 0; index < class_instance_count; ++index) {
        if (class_instances[index]) {
            class_instances[index]->Release();
        }
    }
    if (!state.original_pixel_shader || class_instance_count != 0) {
        return false;
    }

    {
        const auto key = reinterpret_cast<std::uintptr_t>(
            state.original_pixel_shader.Get());
        std::scoped_lock lock(data.instrumented_shader_mutex);
        const auto found = data.instrumented_pixel_shaders.find(key);
        if (found != data.instrumented_pixel_shaders.end()) {
            state.instrumented_pixel_shader = found->second;
        }
    }
    for (std::size_t index = 0; index < state.render_targets.size(); ++index) {
        state.render_targets[index] = all_render_targets[index];
    }
    state.depth_stencil = depth_stencil;
    state.depth_state = depth_state;
    state.stencil_reference = stencil_reference;
    state.blend_state = blend_state;
    state.blend_factor = blend_factor;
    state.sample_mask = sample_mask;
    return true;
}

void InvalidateCaptureFrame(DeviceData& data, const char* reason) noexcept {
    data.capture_valid = false;
    ++data.capture_misses;
    if (!data.logged_capture_failure) {
        data.logged_capture_failure = true;
        Log(reshade::log::level::warning,
            "[ShenLong-SDAO] stochastic depth capture became incomplete (%s); native AO remains active for the frame.",
            reason);
    }
}

bool BeginCaptureFrame(
    ID3D11DeviceContext* context,
    DeviceData& data,
    std::uint32_t width,
    std::uint32_t height) {
    const std::uint64_t frame =
        g_present_count.load(std::memory_order_relaxed);
    if (data.capture_started && data.capture_frame == frame) {
        return data.capture_valid;
    }

    if (!CreateStochasticDepth(
            data.native_device, data, width, height)) {
        data.capture_frame = frame;
        data.capture_started = true;
        data.capture_valid = false;
        data.capture_draws = 0;
        data.capture_misses = 0;
        InvalidateCaptureFrame(data, "resource allocation failed");
        return false;
    }

    data.capture_frame = frame;
    data.capture_started = true;
    data.capture_valid = false;
    data.capture_draws = 0;
    data.capture_misses = 0;

    ComPtr<ID3D11Predicate> predicate;
    BOOL predicate_value = FALSE;
    context->GetPredication(predicate.GetAddressOf(), &predicate_value);
    context->SetPredication(nullptr, FALSE);
    constexpr std::array<FLOAT, 4> clear_values = {
        1.0f, 1.0f, 1.0f, 1.0f};
    for (std::uint32_t pair = 0; pair < data.stochastic_pairs; ++pair) {
        context->ClearRenderTargetView(
            data.stochastic_depth_rtvs[pair].Get(), clear_values.data());
    }
    context->SetPredication(predicate.Get(), predicate_value);
    data.capture_valid = true;
    return true;
}

bool DrawWithStochasticCapture(
    ID3D11DeviceContext* context,
    DeviceData& data,
    const CameraDrawState& state,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) {
    if (!BeginCaptureFrame(context, data, state.width, state.height) ||
        !data.capture_valid) {
        return false;
    }
    if (state.uav_conflict) {
        InvalidateCaptureFrame(data, "camera OM UAV slots 3-7 were occupied");
        return false;
    }
    if (!state.instrumented_pixel_shader || !data.capture_depth_state ||
        !data.capture_min_blend_state || data.stochastic_pairs == 0) {
        InvalidateCaptureFrame(data, "camera pixel shader was not instrumentable");
        return false;
    }

    std::array<ID3D11RenderTargetView*, 3> render_targets = {
        state.render_targets[0].Get(),
        state.render_targets[1].Get(),
        state.render_targets[2].Get()};
    std::array<ID3D11RenderTargetView*, 2> capture_targets{};
    for (std::uint32_t pair = 0; pair < data.stochastic_pairs; ++pair) {
        capture_targets[pair] = data.stochastic_depth_rtvs[pair].Get();
    }
    context->PSSetShader(state.instrumented_pixel_shader.Get(), nullptr, 0);
    context->OMSetDepthStencilState(data.capture_depth_state.Get(), 0);
    constexpr std::array<FLOAT, 4> capture_blend_factor = {
        1.0f, 1.0f, 1.0f, 1.0f};
    context->OMSetBlendState(
        data.capture_min_blend_state.Get(),
        capture_blend_factor.data(),
        state.sample_mask);
    context->OMSetRenderTargets(
        data.stochastic_pairs, capture_targets.data(), nullptr);

    const auto draw = [&]() {
        if (instance_count == 1 && first_instance == 0) {
            context->DrawIndexed(index_count, first_index, vertex_offset);
        } else {
            context->DrawIndexedInstanced(
                index_count,
                instance_count,
                first_index,
                vertex_offset,
                first_instance);
        }
    };

    // Replay only the alpha-coverage slice at full resolution. Fixed-function
    // MIN blending preserves the exact nearest selected depth per stochastic
    // layer without a UAV atomic or native MRT/depth write. The original draw
    // then runs with untouched state and keeps the game's early-depth path.
    g_running_capture_draw = true;
    draw();

    std::array<ID3D11UnorderedAccessView*, 5> old_uavs{};
    for (std::size_t index = 0; index < old_uavs.size(); ++index) {
        old_uavs[index] = state.old_uavs[index].Get();
    }
    constexpr std::array<UINT, 5> keep_counters = {
        D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        D3D11_KEEP_UNORDERED_ACCESS_VIEWS};
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        static_cast<UINT>(render_targets.size()),
        render_targets.data(),
        state.depth_stencil.Get(),
        3,
        static_cast<UINT>(old_uavs.size()),
        old_uavs.data(),
        keep_counters.data());
    context->OMSetDepthStencilState(
        state.depth_state.Get(), state.stencil_reference);
    context->OMSetBlendState(
        state.blend_state.Get(), state.blend_factor.data(), state.sample_mask);
    context->PSSetShader(state.original_pixel_shader.Get(), nullptr, 0);
    draw();
    g_running_capture_draw = false;
    ++data.capture_draws;
    return true;
}

bool IsSupportedDepthFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
           format == DXGI_FORMAT_R32_FLOAT ||
           format == DXGI_FORMAT_R16_UNORM ||
           format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

void InvalidateNativeAoConstants(DeviceData& data) noexcept {
    data.native_ao_constants_valid = false;
    data.native_ao_constants_frame =
        (std::numeric_limits<std::uint64_t>::max)();
}

bool CaptureNativeAoConstants(ID3D11DeviceContext* context, DeviceData& data) {
    ComPtr<ID3D11Buffer> source;
    context->CSGetConstantBuffers(0, 1, source.GetAddressOf());
    if (!source || !data.native_device) {
        InvalidateNativeAoConstants(data);
        return false;
    }

    D3D11_BUFFER_DESC source_desc{};
    source->GetDesc(&source_desc);
    constexpr UINT kRequiredProjectionBytes =
        static_cast<UINT>(sizeof(std::array<float, 16>));
    if (source_desc.ByteWidth < kRequiredProjectionBytes ||
        (source_desc.ByteWidth % 16u) != 0 ||
        (source_desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) {
        InvalidateNativeAoConstants(data);
        return false;
    }

    if (!data.native_ao_constants ||
        data.native_ao_constants_size != source_desc.ByteWidth) {
        D3D11_BUFFER_DESC destination_desc{};
        destination_desc.ByteWidth = source_desc.ByteWidth;
        destination_desc.Usage = D3D11_USAGE_DEFAULT;
        destination_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(data.native_device->CreateBuffer(
                &destination_desc,
                nullptr,
                data.native_ao_constants.ReleaseAndGetAddressOf()))) {
            data.native_ao_constants_size = 0;
            InvalidateNativeAoConstants(data);
            return false;
        }
        data.native_ao_constants_size = source_desc.ByteWidth;
    }

    // Copy/resolve commands inherit D3D11 predication. Preserve the game's
    // predicate for the stock AO dispatch, but disable it for this mandatory
    // snapshot so a false predicate cannot leave stale constants marked as
    // current.
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicate_value = FALSE;
    context->GetPredication(predicate.GetAddressOf(), &predicate_value);
    context->SetPredication(nullptr, FALSE);
    context->CopyResource(data.native_ao_constants.Get(), source.Get());
    context->SetPredication(predicate.Get(), predicate_value);
    data.native_ao_constants_valid = true;
    data.native_ao_constants_frame =
        g_present_count.load(std::memory_order_relaxed);
    return true;
}

#if defined(SPATCH_SDAO_DEVELOPMENT)
void LogCompositionBindings(
    ID3D11DeviceContext* context,
    DeviceData& data,
    ID3D11ShaderResourceView* depth,
    const D3D11_TEXTURE2D_DESC& depth_desc,
    ID3D11ShaderResourceView* original_ao) {
    if (data.composition_bindings_logged) {
        return;
    }
    data.composition_bindings_logged = true;

    D3D11_SHADER_RESOURCE_VIEW_DESC depth_view_desc{};
    D3D11_SHADER_RESOURCE_VIEW_DESC ao_view_desc{};
    D3D11_TEXTURE2D_DESC ao_desc{};
    depth->GetDesc(&depth_view_desc);
    original_ao->GetDesc(&ao_view_desc);
    const bool ao_desc_valid = GetTextureDescription(original_ao, ao_desc);

    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports{};
    context->RSGetViewports(&viewport_count, viewports.data());

    UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_RECT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        scissors{};
    context->RSGetScissorRects(&scissor_count, scissors.data());

    ComPtr<ID3D11RenderTargetView> render_target;
    context->OMGetRenderTargets(1, render_target.GetAddressOf(), nullptr);
    D3D11_TEXTURE2D_DESC render_target_desc{};
    bool render_target_desc_valid = false;
    if (render_target) {
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        render_target->GetResource(resource.GetAddressOf());
        if (resource && SUCCEEDED(resource.As(&texture))) {
            texture->GetDesc(&render_target_desc);
            render_target_desc_valid = true;
        }
    }

    ComPtr<ID3D11SamplerState> ao_sampler;
    context->PSGetSamplers(7, 1, ao_sampler.GetAddressOf());
    D3D11_SAMPLER_DESC ao_sampler_desc{
        D3D11_FILTER_MIN_MAG_MIP_POINT,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        0.0f,
        1u,
        D3D11_COMPARISON_NEVER,
        {0.0f, 0.0f, 0.0f, 0.0f},
        0.0f,
        0.0f,
    };
    if (ao_sampler) {
        ao_sampler->GetDesc(&ao_sampler_desc);
    }

    ComPtr<ID3D11RasterizerState> rasterizer;
    context->RSGetState(rasterizer.GetAddressOf());
    D3D11_RASTERIZER_DESC rasterizer_desc{
        D3D11_FILL_SOLID,
        D3D11_CULL_BACK,
        FALSE,
        0,
        0.0f,
        0.0f,
        TRUE,
        FALSE,
        FALSE,
        FALSE,
    };
    if (rasterizer) {
        rasterizer->GetDesc(&rasterizer_desc);
    }

    Log(reshade::log::level::info,
        "[ShenLong-SDAO] composition bindings: depth=%ux%u resource_format=%u srv_format=%u; original_ao=%ux%u resource_format=%u srv_format=%u mip=%u levels=%u; target=%ux%u format=%u.",
        depth_desc.Width,
        depth_desc.Height,
        static_cast<unsigned int>(depth_desc.Format),
        static_cast<unsigned int>(depth_view_desc.Format),
        ao_desc_valid ? ao_desc.Width : 0u,
        ao_desc_valid ? ao_desc.Height : 0u,
        ao_desc_valid ? static_cast<unsigned int>(ao_desc.Format) : 0u,
        static_cast<unsigned int>(ao_view_desc.Format),
        ao_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D
            ? ao_view_desc.Texture2D.MostDetailedMip
            : 0u,
        ao_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D
            ? ao_view_desc.Texture2D.MipLevels
            : 0u,
        render_target_desc_valid ? render_target_desc.Width : 0u,
        render_target_desc_valid ? render_target_desc.Height : 0u,
        render_target_desc_valid
            ? static_cast<unsigned int>(render_target_desc.Format)
            : 0u);
    if (viewport_count != 0) {
        const D3D11_VIEWPORT& viewport = viewports[0];
        Log(reshade::log::level::info,
            "[ShenLong-SDAO] composition raster: viewports=%u first=(%.1f,%.1f %.1fx%.1f z=%.3f..%.3f); scissors=%u first=(%ld,%ld)-(%ld,%ld); scissor_enable=%u; ao_sampler_filter=%u address=%u/%u/%u.",
            viewport_count,
            viewport.TopLeftX,
            viewport.TopLeftY,
            viewport.Width,
            viewport.Height,
            viewport.MinDepth,
            viewport.MaxDepth,
            scissor_count,
            scissor_count != 0 ? scissors[0].left : 0l,
            scissor_count != 0 ? scissors[0].top : 0l,
            scissor_count != 0 ? scissors[0].right : 0l,
            scissor_count != 0 ? scissors[0].bottom : 0l,
            rasterizer ? (rasterizer_desc.ScissorEnable ? 1u : 0u) : 0u,
            ao_sampler ? static_cast<unsigned int>(ao_sampler_desc.Filter) : 0u,
            ao_sampler ? static_cast<unsigned int>(ao_sampler_desc.AddressU) : 0u,
            ao_sampler ? static_cast<unsigned int>(ao_sampler_desc.AddressV) : 0u,
            ao_sampler ? static_cast<unsigned int>(ao_sampler_desc.AddressW) : 0u);
    }
}

void LogNativeAoBindings(ID3D11DeviceContext* context, DeviceData& data) {
    if (data.native_ao_bindings_logged) {
        return;
    }
    data.native_ao_bindings_logged = true;

    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11UnorderedAccessView> output;
    ComPtr<ID3D11Buffer> constants;
    context->CSGetShaderResources(0, 1, depth.GetAddressOf());
    context->CSGetUnorderedAccessViews(0, 1, output.GetAddressOf());
    context->CSGetConstantBuffers(0, 1, constants.GetAddressOf());
    if (!depth || !output || !constants) {
        Log(reshade::log::level::warning,
            "[ShenLong-SDAO] native AO development bindings were incomplete.");
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC depth_view_desc{};
    D3D11_UNORDERED_ACCESS_VIEW_DESC output_view_desc{};
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth->GetDesc(&depth_view_desc);
    output->GetDesc(&output_view_desc);
    if (!GetTextureDescription(depth.Get(), depth_desc)) {
        return;
    }
    Log(reshade::log::level::info,
        "[ShenLong-SDAO] native AO bindings: %ux%u depth_srv_format=%u output_uav_format=%u.",
        depth_desc.Width,
        depth_desc.Height,
        static_cast<unsigned int>(depth_view_desc.Format),
        static_cast<unsigned int>(output_view_desc.Format));

    D3D11_BUFFER_DESC source_desc{};
    constants->GetDesc(&source_desc);
    D3D11_BUFFER_DESC staging_desc = source_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> staging;
    if (!data.native_device || FAILED(data.native_device->CreateBuffer(
            &staging_desc, nullptr, staging.ReleaseAndGetAddressOf()))) {
        return;
    }
    context->CopyResource(staging.Get(), constants.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) ||
        !mapped.pData) {
        return;
    }

    const auto* values = static_cast<const float*>(mapped.pData);
    const std::size_t vector_count = (std::min)(
        static_cast<std::size_t>(source_desc.ByteWidth / (4u * sizeof(float))),
        std::size_t{4});
    for (std::size_t index = 0; index < vector_count; ++index) {
        Log(reshade::log::level::info,
            "[ShenLong-SDAO] cbNativeAO[%zu]=%.9g,%.9g,%.9g,%.9g",
            index,
            values[index * 4u],
            values[index * 4u + 1u],
            values[index * 4u + 2u],
            values[index * 4u + 3u]);
    }
    context->Unmap(staging.Get(), 0);
}
#endif

bool ReadCompositionBindings(
    ID3D11DeviceContext* context,
    DeviceData& data,
    ComPtr<ID3D11ShaderResourceView>& original_ao,
    ComPtr<ID3D11ShaderResourceView>& full_depth,
    D3D11_TEXTURE2D_DESC& full_depth_desc) {
    context->PSGetShaderResources(2, 1, full_depth.GetAddressOf());
    context->PSGetShaderResources(7, 1, original_ao.GetAddressOf());
    if (!full_depth || !original_ao || !HasFreshNativeAoConstants(data)) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC depth_view_desc{};
    full_depth->GetDesc(&depth_view_desc);
    if (!IsSupportedDepthFormat(depth_view_desc.Format)) {
        return false;
    }

    if (!GetTextureDescription(full_depth.Get(), full_depth_desc) ||
        full_depth_desc.ArraySize != 1 || full_depth_desc.SampleDesc.Count != 1 ||
        full_depth_desc.Width < 16 || full_depth_desc.Height < 16) {
        return false;
    }

#if defined(SPATCH_SDAO_DEVELOPMENT)
    LogCompositionBindings(
        context, data, full_depth.Get(), full_depth_desc, original_ao.Get());
#endif

    return data.native_device && CreateScratch(
        data.native_device, data, full_depth_desc.Width, full_depth_desc.Height);
}

void ClearComputeBindings(ID3D11DeviceContext* context) noexcept {
    static constexpr std::array<ID3D11ShaderResourceView*, 3> null_srvs{};
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context->CSSetShaderResources(
        0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

#if defined(SPATCH_SDAO_DEVELOPMENT)
void RecordGpuTiming(DeviceData& data, double milliseconds) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
        data.gpu_timing_logged ||
        data.gpu_timing_sample_count >= data.gpu_timing_samples.size()) {
        return;
    }
    data.gpu_timing_samples[data.gpu_timing_sample_count++] = milliseconds;
    if (data.gpu_timing_sample_count != data.gpu_timing_samples.size()) {
        return;
    }

    std::array<double, 120> sorted = data.gpu_timing_samples;
    std::sort(sorted.begin(), sorted.end());
    const double average = std::accumulate(
        sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    const std::size_t p95_index =
        static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1));
    data.gpu_timing_logged = true;
    Log(reshade::log::level::info,
        "[ShenLong-SDAO] GPU timing at %ux%u: average=%.3f ms, p95=%.3f ms, maximum=%.3f ms (%zu frames).",
        data.width,
        data.height,
        average,
        sorted[p95_index],
        sorted.back(),
        sorted.size());
}

bool ConsumeGpuTiming(
    ID3D11DeviceContext* context,
    DeviceData& data,
    GpuTimingSlot& slot) noexcept {
    if (!slot.pending) {
        return true;
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 start = 0;
    UINT64 end = 0;
    constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (context->GetData(
            slot.disjoint.Get(), &disjoint, sizeof(disjoint), flags) != S_OK ||
        context->GetData(slot.start.Get(), &start, sizeof(start), flags) != S_OK ||
        context->GetData(slot.end.Get(), &end, sizeof(end), flags) != S_OK) {
        return false;
    }
    slot.pending = false;
    if (!disjoint.Disjoint && disjoint.Frequency != 0 && end >= start) {
        RecordGpuTiming(
            data,
            static_cast<double>(end - start) * 1000.0 /
                static_cast<double>(disjoint.Frequency));
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
            context_->End(slot_->start.Get());
            return;
        }
    }

    GpuTimingScope(const GpuTimingScope&) = delete;
    GpuTimingScope& operator=(const GpuTimingScope&) = delete;

    ~GpuTimingScope() {
        if (!slot_) {
            return;
        }
        context_->End(slot_->end.Get());
        context_->End(slot_->disjoint.Get());
        slot_->pending = true;
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    DeviceData& data_;
    GpuTimingSlot* slot_ = nullptr;
};
#endif

bool RunSelectedAo(
    ID3D11DeviceContext* context,
    DeviceData& data,
    ComPtr<ID3D11ShaderResourceView>& original_ao) {
    const std::uint64_t frame =
        g_present_count.load(std::memory_order_relaxed);
    if (!IsStandaloneRuntimeEnabled(data) ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }
    const bool sdao = IsSdaoSelected(data.settings);
    if (sdao && (!data.capture_started || !data.capture_valid ||
        data.capture_frame != frame || data.capture_draws == 0 ||
        !data.stochastic_depth_srv)) {
        return false;
    }

    ComPtr<ID3D11ShaderResourceView> full_depth;
    D3D11_TEXTURE2D_DESC depth_desc{};
    if (!ReadCompositionBindings(
            context,
            data,
            original_ao,
            full_depth,
            depth_desc)) {
        return false;
    }
    if (sdao && (data.stochastic_width != depth_desc.Width ||
        data.stochastic_height != depth_desc.Height ||
        data.stochastic_layers !=
            kStochasticLayerCounts[static_cast<std::size_t>(data.settings.quality)])) {
        return false;
    }

#if defined(SPATCH_SDAO_DEVELOPMENT)
    GpuTimingScope gpu_timing(context, data);
#endif
    ScopedComputeState state(context);
    ClearComputeBindings(context);

    ID3D11Buffer* projection_constants = data.native_ao_constants.Get();
    ID3D11Buffer* sdao_constants = data.constants.Get();
    ID3D11SamplerState* point_sampler = data.point_sampler.Get();
    context->CSSetConstantBuffers(9, 1, &projection_constants);
    context->CSSetConstantBuffers(11, 1, &sdao_constants);
    context->CSSetSamplers(0, 1, &point_sampler);

    // Convert the full-resolution device depth to positive view-space metres.
    ID3D11ShaderResourceView* input = full_depth.Get();
    ID3D11UnorderedAccessView* output = data.linear_depth_uav.Get();
    context->CSSetShaderResources(0, 1, &input);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(data.prepare_depth_shader.Get(), nullptr, 0);
    context->Dispatch((data.width + 7u) / 8u, (data.height + 7u) / 8u, 1);

    // Evaluate the conventional front-depth horizon at final-composition
    // resolution and, for SDAO only, sample every full-resolution stochastic
    // hidden-depth layer. GTAO Lite compiles the hidden-layer branch out and
    // never allocates or captures it.
    // Coarse depth mips require temporal accumulation to hide block transitions;
    // this integration point exposes neither motion vectors nor AO history.
    ClearComputeBindings(context);
    std::array<ID3D11ShaderResourceView*, 3> main_inputs = {
        data.linear_depth_srv.Get(), nullptr,
        sdao ? data.stochastic_depth_srv.Get() : nullptr};
    output = data.raw_ao_uav.Get();
    context->CSSetShaderResources(
        0, static_cast<UINT>(main_inputs.size()), main_inputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(data.main_shader.Get(), nullptr, 0);
    context->Dispatch((data.width + 7u) / 8u, (data.height + 7u) / 8u, 1);

    // Coherent radial sampling preserves texture-cache locality. The main pass
    // rejects isolated thin-occluder horizons, then the separable cross-bilateral
    // pass reconstructs the result without temporal history or frame noise.
    ClearComputeBindings(context);
    std::array<ID3D11ShaderResourceView*, 2> filter_inputs = {
        data.raw_ao_srv.Get(), data.linear_depth_srv.Get()};
    output = data.filtered_ao_uav.Get();
    context->CSSetShaderResources(
        0,
        static_cast<UINT>(filter_inputs.size()),
        filter_inputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(data.horizontal_filter_shader.Get(), nullptr, 0);
    context->Dispatch((data.width + 7u) / 8u, (data.height + 7u) / 8u, 1);

    ClearComputeBindings(context);
    filter_inputs[0] = data.filtered_ao_srv.Get();
    output = data.final_ao_uav.Get();
    context->CSSetShaderResources(
        0,
        static_cast<UINT>(filter_inputs.size()),
        filter_inputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(data.vertical_filter_shader.Get(), nullptr, 0);
    context->Dispatch((data.width + 7u) / 8u, (data.height + 7u) / 8u, 1);
    ClearComputeBindings(context);
    return true;
}

void RecordAoReplacementSuccess(DeviceData& data) {
    ++data.replacement_frames;
    data.consecutive_composition_failures = 0;
    data.logged_failure = false;

    if (IsSdaoSelected(data.settings)) {
        ++data.captured_frames;
        if (!data.logged_capture_success) {
            data.logged_capture_success = true;
            Log(reshade::log::level::info,
                "[ShenLong-SDAO] stochastic capture active: frame=%llu draws=%u misses=%u layers=%u resolution=%ux%u alpha=0.2 path=full-resolution-min-blend-replay capture-shaders=%llu fallback-shaders=%llu rejected-shaders=%llu.",
                static_cast<unsigned long long>(data.capture_frame),
                data.capture_draws,
                data.capture_misses,
                data.stochastic_layers,
                data.stochastic_width,
                data.stochastic_height,
                static_cast<unsigned long long>(
                    data.instrumented_shader_count.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data.contiguous_fallback_shader_count.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data.rejected_shader_count.load(std::memory_order_relaxed)));
#if defined(SPATCH_SDAO_DEVELOPMENT)
            Log(reshade::log::level::info,
                "[ShenLong-SDAO] immutable camera attachment cache: cache_hits=%llu cache_miss_count=%llu.",
                static_cast<unsigned long long>(data.camera_attachment_cache_hits),
                static_cast<unsigned long long>(data.camera_attachment_cache_misses));
#endif
        }
    }

    if (!data.logged_success) {
        data.logged_success = true;
        if (IsSdaoSelected(data.settings)) {
            Log(reshade::log::level::info,
                "[ShenLong-SDAO] full-resolution SDAO active at %ux%u; quality=%d layers=%u capture-draws=%u, full-resolution alpha-only MIN-blend replay preserves native early depth/MRT/depth without UAV atomics, DiligentFX-derived horizon evaluation and cross-bilateral reconstruction enabled.",
                data.width,
                data.height,
                data.settings.quality,
                data.stochastic_layers,
                data.capture_draws);
        } else {
            Log(reshade::log::level::info,
                "[ShenLong-AO] GTAO Lite active at %ux%u; quality=%d, front-depth horizon evaluation and cross-bilateral reconstruction enabled with no stochastic capture overhead.",
                data.width,
                data.height,
                data.settings.quality);
        }
    }
}

void RecordAoReplacementFailure(DeviceData& data) {
    constexpr std::uint32_t kFailureLogThreshold = 120;
    if (data.consecutive_composition_failures < kFailureLogThreshold) {
        ++data.consecutive_composition_failures;
    }
    if (data.consecutive_composition_failures == kFailureLogThreshold &&
        !data.logged_failure) {
        data.logged_failure = true;
        Log(reshade::log::level::warning,
            "[ShenLong-AO] %s inputs were unavailable for 120 consecutive composition draws; the game's Original AO remains active as the same-frame fallback.",
            IsSdaoSelected(data.settings) ?
                "SDAO stochastic capture or composition" :
                "GTAO Lite composition");
    }
}

bool DrawFinalCompositionWithSelectedAo(
    ID3D11DeviceContext* context,
    DeviceData& data,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) {
    ComPtr<ID3D11ShaderResourceView> original_ao;
    if (!RunSelectedAo(context, data, original_ao)) {
        return false;
    }

    ID3D11ShaderResourceView* replacement_ao = data.final_ao_srv.Get();
    context->PSSetShaderResources(7, 1, &replacement_ao);
    g_running_composition = true;
    if (instance_count == 1 && first_instance == 0) {
        context->DrawIndexed(index_count, first_index, vertex_offset);
    } else {
        context->DrawIndexedInstanced(
            index_count,
            instance_count,
            first_index,
            vertex_offset,
            first_instance);
    }
    g_running_composition = false;

    ID3D11ShaderResourceView* restored_ao = original_ao.Get();
    context->PSSetShaderResources(7, 1, &restored_ao);
    RecordAoReplacementSuccess(data);
    return true;
}

void OnInitDevice(reshade::api::device* device) {
    DeviceData* data =
        spatch::graphics::detail::CreatePrivateData<DeviceData>(device);
    if (!data) {
        return;
    }
    data->settings = LoadSettings();
    if (!NeedsAoInterception(data->settings, device)) {
        return;
    }
    if (device->get_api() != reshade::api::device_api::d3d11) {
        Log(reshade::log::level::warning,
            "[ShenLong-SDAO] Unsupported graphics API; retaining Original AO.");
        return;
    }

    data->native_device = NativePointer<ID3D11Device>(device->get_native());
    data->ready = data->settings.mode != AoMode::Original &&
        data->native_device && InitializeShaders(data->native_device, *data);
    if (data->settings.mode != AoMode::Original && !data->ready) {
        Log(reshade::log::level::warning,
            "[ShenLong-SDAO] Initialization failed; retaining Original AO.");
    }
}

void OnDestroyDevice(reshade::api::device* device) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (data) {
        std::unordered_map<std::uintptr_t, ComPtr<ID3D11PixelShader>>
            replacements;
        {
            std::scoped_lock lock(data->instrumented_shader_mutex);
            replacements.swap(data->instrumented_pixel_shaders);
        }

        // Shader Release raises destroy_pipeline synchronously. Drain every
        // owned pipeline while DeviceData and its now-empty map/mutex are
        // still alive; ReShade clears the private-data pointer only after the
        // DeviceData destructor has returned.
        replacements.clear();
        data->prepare_depth_shader.Reset();
        data->main_shader.Reset();
        data->horizontal_filter_shader.Reset();
        data->vertical_filter_shader.Reset();
    }
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
    if (g_creating_instrumented_shader ||
        device->get_api() != reshade::api::device_api::d3d11 || pipeline.handle == 0 ||
        (subobjects == nullptr && subobject_count != 0)) {
        return;
    }
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!data) {
        return;
    }
    const bool needs_ao = NeedsAoInterception(data->settings, device);
    if (!needs_ao) {
        return;
    }

    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto& subobject = subobjects[index];
        const bool is_compute_shader =
            subobject.type == reshade::api::pipeline_subobject_type::compute_shader;
        const bool is_pixel_shader =
            subobject.type == reshade::api::pipeline_subobject_type::pixel_shader;
        if ((!is_compute_shader && !is_pixel_shader) ||
            subobject.count == 0 || !subobject.data) {
            continue;
        }
        const auto* descriptions =
            static_cast<const reshade::api::shader_desc*>(subobject.data);
        for (std::uint32_t shader_index = 0; shader_index < subobject.count; ++shader_index) {
            const auto& description = descriptions[shader_index];
            if (!description.code || description.code_size == 0) {
                continue;
            }
            const std::uint32_t hash = compute_crc32(
                static_cast<const std::uint8_t*>(description.code),
                description.code_size);
            if (is_pixel_shader) {
                auto* native_shader = NativePointer<ID3D11PixelShader>(pipeline.handle);
                if (hash == kFinalCompositionPixelShaderHash) {
                    const HRESULT result = native_shader->SetPrivateData(
                        kPixelShaderHashTag, sizeof(hash), &hash);
                    if (FAILED(result)) {
                        Log(reshade::log::level::warning,
                            "[ShenLong-SDAO] Could not tag the final composition shader (HRESULT=0x%08X).",
                            static_cast<unsigned int>(result));
                    }
                    return;
                }

                if (!IsSdaoSelected(data->settings) || !data->ready ||
                    data->capture_tail_bytecode.empty()) {
                    return;
                }
                auto instrumented =
                    spatch::graphics::sdao::dxbc::InstrumentPixelShader(
                        description.code,
                        description.code_size,
                        data->capture_tail_bytecode.data(),
                        data->capture_tail_bytecode.size());
                if (!instrumented) {
                    ++data->rejected_shader_count;
                    return;
                }

                ComPtr<ID3D11PixelShader> replacement;
                g_creating_instrumented_shader = true;
                const HRESULT create_result = data->native_device->CreatePixelShader(
                    instrumented.bytecode.data(),
                    instrumented.bytecode.size(),
                    nullptr,
                    replacement.ReleaseAndGetAddressOf());
                g_creating_instrumented_shader = false;
                if (FAILED(create_result) || !replacement) {
                    ++data->rejected_shader_count;
                    return;
                }
                const auto pipeline_key =
                    static_cast<std::uintptr_t>(pipeline.handle);
                {
                    std::scoped_lock lock(data->instrumented_shader_mutex);
                    const auto [entry, inserted] =
                        data->instrumented_pixel_shaders.emplace(
                            pipeline_key, replacement);
                    static_cast<void>(entry);
                    if (!inserted) {
                        return;
                    }
                }
                constexpr std::uint8_t instrumentable = 1;
                const HRESULT tag_result = native_shader->SetPrivateData(
                    kInstrumentablePixelShaderTag,
                    sizeof(instrumentable),
                    &instrumentable);
                if (FAILED(tag_result)) {
                    ComPtr<ID3D11PixelShader> rejected;
                    {
                        std::scoped_lock lock(data->instrumented_shader_mutex);
                        const auto entry =
                            data->instrumented_pixel_shaders.find(pipeline_key);
                        if (entry != data->instrumented_pixel_shaders.end()) {
                            rejected = std::move(entry->second);
                            data->instrumented_pixel_shaders.erase(entry);
                        }
                    }
                    ++data->rejected_shader_count;
                    Log(reshade::log::level::warning,
                        "[ShenLong-SDAO] Could not tag an instrumentable camera shader (HRESULT=0x%08X).",
                        static_cast<unsigned int>(tag_result));
                    return;
                }
                if (instrumented.used_contiguous_fallback) {
                    ++data->contiguous_fallback_shader_count;
                }
                ++data->instrumented_shader_count;
                return;
            }

            const AoPass pass = ClassifyShaderHash(hash);
            if (pass == AoPass::None) {
                continue;
            }

            auto* native_shader = NativePointer<ID3D11ComputeShader>(pipeline.handle);
            const HRESULT result =
                native_shader->SetPrivateData(kAoPassTag, sizeof(pass), &pass);
            if (FAILED(result)) {
                Log(reshade::log::level::warning,
                    "[ShenLong-SDAO] Could not tag a stock AO shader (HRESULT=0x%08X).",
                    static_cast<unsigned int>(result));
            }
            return;
        }
    }
}

void OnDestroyPipeline(
    reshade::api::device* device,
    reshade::api::pipeline pipeline) {
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!data || pipeline.handle == 0) {
        return;
    }

    ComPtr<ID3D11PixelShader> replacement;
    {
        std::scoped_lock lock(data->instrumented_shader_mutex);
        const auto shader = data->instrumented_pixel_shaders.find(
            static_cast<std::uintptr_t>(pipeline.handle));
        if (shader != data->instrumented_pixel_shaders.end()) {
            replacement = std::move(shader->second);
            data->instrumented_pixel_shaders.erase(shader);
        }
    }
    // Releasing the replacement synchronously raises destroy_pipeline for that
    // replacement. Keep the release outside the non-recursive map mutex.
}

void OnBindPipeline(
    reshade::api::command_list* command_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline pipeline) {
    CommandListData* command_data =
        command_list->get_private_data<CommandListData>();
    if (!command_data) {
        return;
    }

    DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!device_data) {
        return;
    }
    const bool needs_ao = NeedsAoInterception(
        device_data->settings, command_list->get_device());

    if (needs_ao &&
        (stages & reshade::api::pipeline_stage::compute_shader) != 0) {
        command_data->current_pass = AoPass::None;
        if (pipeline.handle != 0) {
            auto* native_shader = NativePointer<ID3D11ComputeShader>(pipeline.handle);
            AoPass pass = AoPass::None;
            UINT size = sizeof(pass);
            if (SUCCEEDED(native_shader->GetPrivateData(kAoPassTag, &size, &pass)) &&
                size == sizeof(pass)) {
                command_data->current_pass = pass;
            }
        }
    }

    if (needs_ao &&
        (stages & reshade::api::pipeline_stage::pixel_shader) != 0) {
        command_data->pixel_shader_hash = 0;
        command_data->pixel_shader_is_instrumentable = false;
        if (pipeline.handle != 0) {
            auto* native_shader = NativePointer<ID3D11PixelShader>(pipeline.handle);
            UINT size = sizeof(command_data->pixel_shader_hash);
            native_shader->GetPrivateData(
                kPixelShaderHashTag, &size, &command_data->pixel_shader_hash);
            std::uint8_t instrumentable = 0;
            size = sizeof(instrumentable);
            if (SUCCEEDED(native_shader->GetPrivateData(
                    kInstrumentablePixelShaderTag,
                    &size,
                    &instrumentable)) &&
                size == sizeof(instrumentable)) {
                command_data->pixel_shader_is_instrumentable =
                    instrumentable != 0;
            }
        }
    }
}

bool OnDispatch(
    reshade::api::command_list* command_list,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t) {
    CommandListData* command_data =
        command_list->get_private_data<CommandListData>();
    if (!command_data || command_data->current_pass == AoPass::None) {
        return false;
    }
#if defined(SPATCH_SDAO_DEVELOPMENT)
    switch (command_data->current_pass) {
        case AoPass::Coarse:
            g_ao_coarse_dispatches_since_present.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case AoPass::Final:
            g_ao_final_dispatches_since_present.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case AoPass::BlurFirst:
            g_ao_blur_first_dispatches_since_present.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case AoPass::BlurSecond:
            g_ao_blur_second_dispatches_since_present.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case AoPass::None:
            break;
    }
#endif
    DeviceData* data = command_list->get_device()->get_private_data<DeviceData>();
    if (!data || !NeedsAoInterception(
            data->settings, command_list->get_device()) || !data->native_device) {
        return false;
    }

    if (command_data->current_pass == AoPass::Final) {
        auto* context = NativePointer<ID3D11DeviceContext>(command_list->get_native());
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            !CaptureNativeAoConstants(context, *data)) {
            return false;
        }
#if defined(SPATCH_SDAO_DEVELOPMENT)
        if (!data->native_ao_bindings_logged) {
            LogNativeAoBindings(context, *data);
        }
#endif
    }

    // Never cancel the stock AO dispatches. The replacement is bound only for
    // the final raw draw, so keeping native AO current costs one stock pass but
    // guarantees a correct same-frame fallback if any later input is missing.
    return false;
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) {
    if (g_running_composition || g_running_capture_draw) {
        return false;
    }
    DeviceData* data = command_list->get_device()->get_private_data<DeviceData>();
    auto* context = NativePointer<ID3D11DeviceContext>(command_list->get_native());
    if (!data || !context ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }
    CommandListData* command_data =
        command_list->get_private_data<CommandListData>();

#if defined(SPATCH_GRAPHICS_UNIFIED)
    const bool gi_enabled =
        spatch::graphics::gi::IsEnabled(command_list->get_device());
#endif
    if (command_data && command_data->pixel_shader_is_instrumentable &&
        IsSdaoSelected(data->settings) && IsStandaloneRuntimeEnabled(*data)) {
        CameraDrawState camera_state;
        bool required_depth_draw = false;
        if (ReadCameraDrawState(
                context, *data, camera_state, required_depth_draw)) {
            return DrawWithStochasticCapture(
                context,
                *data,
                camera_state,
                index_count,
                instance_count,
                first_index,
                vertex_offset,
                first_instance);
        }
        if (required_depth_draw) {
            if (BeginCaptureFrame(
                    context, *data, camera_state.width, camera_state.height)) {
                InvalidateCaptureFrame(
                    *data, "camera draw used an unsupported graphics state");
            }
            return false;
        }
    }

    if (!command_data ||
        command_data->pixel_shader_hash != kFinalCompositionPixelShaderHash) {
        return false;
    }

#if defined(SPATCH_GRAPHICS_UNIFIED)
    if (gi_enabled) {
        const bool custom_ao_ready = IsStandaloneRuntimeEnabled(*data) &&
            HasFreshNativeAoConstants(*data);
        ComPtr<ID3D11ShaderResourceView> original_ao;
        const bool custom_ao_prepared = custom_ao_ready &&
            RunSelectedAo(context, *data, original_ao);
        ID3D11ShaderResourceView* ambient_occlusion_override =
            custom_ao_prepared ? data->final_ao_srv.Get() : nullptr;
        g_running_composition = true;
        const bool gi_drawn = spatch::graphics::gi::DrawPreparedComposition(
            command_list,
            ambient_occlusion_override,
            index_count,
            instance_count,
            first_index,
            vertex_offset,
            first_instance);
        g_running_composition = false;
        if (gi_drawn) {
            if (custom_ao_prepared) {
                RecordAoReplacementSuccess(*data);
            } else if (custom_ao_ready) {
                RecordAoReplacementFailure(*data);
            }
#if defined(SPATCH_SDAO_DEVELOPMENT)
            g_composition_draws_since_present.fetch_add(
                1, std::memory_order_relaxed);
#endif
            return true;
        }
    }
#endif

    const bool custom_ao_ready = IsStandaloneRuntimeEnabled(*data) &&
        HasFreshNativeAoConstants(*data);
#if defined(SPATCH_SDAO_DEVELOPMENT)
    if (custom_ao_ready) {
        g_composition_draws_since_present.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    if (custom_ao_ready &&
        DrawFinalCompositionWithSelectedAo(
            context,
            *data,
            index_count,
            instance_count,
            first_index,
            vertex_offset,
            first_instance)) {
        return true;
    }

    if (custom_ao_ready) {
        RecordAoReplacementFailure(*data);
    }

    return false;
}

#if defined(SPATCH_SDAO_DEVELOPMENT)
void RecordCameraStateCpuTiming(DeviceData& data) noexcept {
    const std::uint64_t ticks = data.camera_state_ticks_since_present;
    const std::uint64_t calls = data.camera_state_calls_since_present;
    data.camera_state_ticks_since_present = 0;
    data.camera_state_calls_since_present = 0;
    if (!data.capture_started || data.camera_state_timing_logged || ticks == 0 ||
        calls == 0 ||
        data.camera_state_timing_sample_count >=
            data.camera_state_frame_milliseconds.size()) {
        return;
    }

    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) == FALSE ||
        frequency.QuadPart <= 0) {
        return;
    }
    data.camera_state_frame_milliseconds[
        data.camera_state_timing_sample_count++] =
        static_cast<double>(ticks) * 1000.0 /
        static_cast<double>(frequency.QuadPart);
    data.camera_state_timing_sample_calls += calls;
    if (data.camera_state_timing_sample_count !=
        data.camera_state_frame_milliseconds.size()) {
        return;
    }

    std::array<double, 120> sorted = data.camera_state_frame_milliseconds;
    std::sort(sorted.begin(), sorted.end());
    const double total_milliseconds = std::accumulate(
        sorted.begin(), sorted.end(), 0.0);
    const double average_milliseconds = total_milliseconds /
        static_cast<double>(sorted.size());
    const std::size_t p95_index =
        static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1));
    const double average_calls =
        static_cast<double>(data.camera_state_timing_sample_calls) /
        static_cast<double>(sorted.size());
    const double average_microseconds_per_call =
        total_milliseconds * 1000.0 /
        static_cast<double>(data.camera_state_timing_sample_calls);
    data.camera_state_timing_logged = true;
    Log(reshade::log::level::info,
        "[ShenLong-SDAO] camera-state CPU timing: average=%.3f ms/frame p95=%.3f ms/frame calls=%.1f/frame average=%.3f us/call (%zu frames).",
        average_milliseconds,
        sorted[p95_index],
        average_calls,
        average_microseconds_per_call,
        sorted.size());
}
#endif

void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    std::uint32_t,
    const reshade::api::rect*) {
    DeviceData* device_data = nullptr;
    if (swapchain) {
        device_data = swapchain->get_device()->get_private_data<DeviceData>();
        if (device_data) {
            for (auto& render_target :
                 device_data->cached_camera_render_targets) {
                render_target.Reset();
            }
            device_data->cached_camera_depth_stencil.Reset();
            device_data->camera_attachments_cached = false;
        }
    }
#if defined(SPATCH_SDAO_DEVELOPMENT)
    if (device_data) {
        RecordCameraStateCpuTiming(*device_data);
    }
#endif
    const std::uint64_t frame =
        g_present_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(SPATCH_SDAO_DEVELOPMENT)
    const std::uint32_t draws =
        g_composition_draws_since_present.exchange(0, std::memory_order_relaxed);
    const std::uint32_t coarse_dispatches =
        g_ao_coarse_dispatches_since_present.exchange(
            0, std::memory_order_relaxed);
    const std::uint32_t final_dispatches =
        g_ao_final_dispatches_since_present.exchange(
            0, std::memory_order_relaxed);
    const std::uint32_t blur_first_dispatches =
        g_ao_blur_first_dispatches_since_present.exchange(
            0, std::memory_order_relaxed);
    const std::uint32_t blur_second_dispatches =
        g_ao_blur_second_dispatches_since_present.exchange(
            0, std::memory_order_relaxed);
    const bool f5_down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5_down && !g_f5_was_down) {
        const bool enabled =
            !g_development_enabled.load(std::memory_order_relaxed);
        g_development_enabled.store(enabled, std::memory_order_relaxed);
        const char* selected_name = device_data &&
                IsGtaoLiteSelected(device_data->settings)
            ? "GTAO Lite"
            : "SDAO";
        Log(reshade::log::level::info,
            "[ShenLong-SDAO] development replacement enabled=%d; %s AO will run on the next frame.",
            enabled ? 1 : 0,
            enabled ? selected_name : "Original");
    }
    g_f5_was_down = f5_down;
    const bool idle_sequence = draws == 0u && coarse_dispatches == 0u &&
        final_dispatches == 0u && blur_first_dispatches == 0u &&
        blur_second_dispatches == 0u;
    const bool stock_ao_sequence = draws <= 1u && coarse_dispatches == 1u &&
        final_dispatches == 1u && blur_first_dispatches == 2u &&
        blur_second_dispatches == 2u;
    // F5 disables only the custom final composition in Development builds;
    // the stock AO chain must keep running so Original remains a live control.
    const bool dispatch_sequence_expected =
        idle_sequence || stock_ao_sequence;
    if (frame <= 16 || !dispatch_sequence_expected) {
        Log(reshade::log::level::info,
            "[ShenLong-SDAO] AO sequence before present %llu: coarse=%u "
            "final=%u blur-first=%u blur-second=%u composition=%u "
            "healthy-1/1/2/2=%d.",
            static_cast<unsigned long long>(frame),
            coarse_dispatches,
            final_dispatches,
            blur_first_dispatches,
            blur_second_dispatches,
            draws,
            dispatch_sequence_expected ? 1 : 0);
    }
#else
    static_cast<void>(frame);
#endif
}

}  // namespace

namespace spatch::graphics::ao {

ID3D11Buffer* GetNativeAoConstants(
    reshade::api::command_list* command_list) noexcept {
    if (!command_list) {
        return nullptr;
    }
    const DeviceData* data =
        command_list->get_device()->get_private_data<DeviceData>();
    return data && HasFreshNativeAoConstants(*data)
        ? data->native_ao_constants.Get()
        : nullptr;
}

void Attach(HMODULE module) {
    g_module = module;
    reshade::register_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::register_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(
        GuardedCallback<OnDestroyPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::init_command_list>(
        GuardedCallback<OnInitCommandList>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_command_list>(
        GuardedCallback<OnDestroyCommandList>::Invoke);
    reshade::register_event<reshade::addon_event::reset_command_list>(
        GuardedCallback<OnResetCommandList>::Invoke);
    reshade::register_event<reshade::addon_event::bind_pipeline>(
        GuardedCallback<OnBindPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::dispatch>(
        GuardedCallback<OnDispatch>::Invoke);
    reshade::register_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
    reshade::register_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
}

void Detach() noexcept {
    reshade::unregister_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
    reshade::unregister_event<reshade::addon_event::dispatch>(
        GuardedCallback<OnDispatch>::Invoke);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(
        GuardedCallback<OnBindPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(
        GuardedCallback<OnResetCommandList>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(
        GuardedCallback<OnDestroyCommandList>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_command_list>(
        GuardedCallback<OnInitCommandList>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(
        GuardedCallback<OnDestroyPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    g_module = nullptr;
}

}  // namespace spatch::graphics::ao

#if !defined(SPATCH_GRAPHICS_UNIFIED)
extern "C" __declspec(dllexport) const char* NAME = "SPatch SDAO";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Full-resolution stochastic-depth AO replacement for Sleeping Dogs: Definitive Edition";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) {
            return FALSE;
        }
        spatch::graphics::ao::Attach(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        spatch::graphics::ao::Detach();
        reshade::unregister_addon(module);
    }
    return TRUE;
}
#endif
