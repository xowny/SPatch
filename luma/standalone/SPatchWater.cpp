// Sleeping Dogs: Definitive Edition water single scattering.
//
// The game has three verified water pixel-shader permutations. This component
// tags only byte-for-byte identities established by CRC, bytecode size, and the
// DXBC checksum, then replays those draws with a precompiled shader which keeps
// the stock water program intact and adds bounded single scattering. Any
// missing shader, setting, identity, or binding leaves the native draw alone.

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <examples/utils/crc32_hash.hpp>
#include <wrl/client.h>

#include "SPatchIni.hpp"
#include "SPatchReShadeCallbackSafety.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSettingsBufferSlot = 13;
constexpr std::uint32_t kAllWaterVariants = (1u << 3) - 1u;

constexpr GUID kWaterShaderTag = {
    0x9f2c27aa,
    0xa376,
    0x4f58,
    {0x8d, 0xb8, 0x3d, 0x37, 0x78, 0x53, 0xd5, 0x45},
};

enum class WaterVariant : std::uint32_t {
    none = 0,
    main = 1,
    simple = 2,
    blend = 3,
};

struct ShaderIdentity {
    WaterVariant variant;
    std::uint32_t native_crc32;
    std::size_t native_bytecode_size;
    std::array<std::uint8_t, 16> native_dxbc_checksum;
    std::uint32_t cache_crc32;
    std::size_t cache_bytecode_size;
    std::array<std::uint8_t, 16> cache_dxbc_checksum;
    const wchar_t* cache_name;
    const wchar_t* source_name;
    const char* display_name;
};

constexpr std::array<ShaderIdentity, 3> kShaderIdentities{{
    {
        WaterVariant::main,
        0x03435804,
        11452,
        {0x28, 0x0A, 0xE4, 0x5F, 0xEB, 0x95, 0x82, 0x70,
         0x2B, 0x7E, 0xDB, 0x4A, 0x17, 0xF9, 0x46, 0xE7},
        0x5035D9CB,
        12196,
        {0x36, 0x43, 0x22, 0x34, 0x3D, 0x6F, 0xEB, 0x45,
         0x41, 0xAA, 0xC2, 0x10, 0x83, 0x86, 0xB5, 0x6E},
        L"WaterMain.ps_4_0.cso",
        L"SPatchWaterMain.hlsl",
        "main",
    },
    {
        WaterVariant::simple,
        0xBD8D0EB0,
        10492,
        {0x50, 0xDD, 0x00, 0x53, 0x83, 0x1A, 0xEC, 0x27,
         0xB0, 0xCB, 0x5E, 0xE8, 0x07, 0x5D, 0x0F, 0x6A},
        0x50CBB227,
        11256,
        {0x69, 0x2F, 0xB6, 0x68, 0x27, 0x6C, 0x92, 0x8E,
         0x93, 0x59, 0xAB, 0xD3, 0x8A, 0xB9, 0x7C, 0xE9},
        L"WaterSimple.ps_4_0.cso",
        L"SPatchWaterSimple.hlsl",
        "simple",
    },
    {
        WaterVariant::blend,
        0xCFCF403E,
        9248,
        {0x41, 0x8D, 0x06, 0xDF, 0x0B, 0x21, 0xC8, 0x0D,
         0x3F, 0xB8, 0xAE, 0xE1, 0x61, 0x01, 0x59, 0xBE},
        0x4F0EA400,
        9984,
        {0xC3, 0x21, 0x45, 0xC4, 0x93, 0xE8, 0x90, 0x10,
         0xED, 0x19, 0x33, 0x29, 0x04, 0x5B, 0xA1, 0x88},
        L"WaterBlend.ps_4_0.cso",
        L"SPatchWaterBlend.hlsl",
        "blend",
    },
}};

struct Settings {
    bool enabled = true;
};

struct alignas(16) WaterConstants {
    float strength;
    float padding0;
    float padding1;
    float padding2;
};

static_assert(sizeof(WaterConstants) == 16);

struct __declspec(uuid("B69B6AA4-0A4D-4C46-B7B5-BEA34883B925")) DeviceData {
    Settings settings;
    bool ready = false;
    std::array<ComPtr<ID3D11PixelShader>, 3> replacement_shaders;
    ComPtr<ID3D11Buffer> settings_buffer;
    std::atomic<std::uint32_t> discovered_variants = 0;
    std::atomic<std::uint32_t> replacement_variants = 0;
    std::array<std::atomic<std::uint64_t>, 3> replacement_draws{};
    std::array<std::atomic<std::uint64_t>, 3> validation_failures{};
#if defined(SPATCH_WATER_DEVELOPMENT)
    std::atomic<std::uint32_t> envmap_telemetry_capture_mask = 0;
    std::atomic<bool> envmap_telemetry_failure_written = false;
#endif
    std::atomic<std::uint64_t> draw_callbacks = 0;
    std::atomic<std::uint64_t> presents = 0;
    std::atomic<bool> registration_warning_written = false;
    std::atomic<bool> validation_warning_written = false;
    std::atomic<bool> success_written = false;
    std::atomic<bool> coverage_written = false;
};

struct DrawCall {
    bool indexed = false;
    std::uint32_t count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
};

HMODULE g_module = nullptr;
thread_local bool g_replaying_draw = false;

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
        "[ShenLong-Water] ReShade callback dropped after %s%s%s; native water remains active.",
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

Settings ReadSettings() {
    Settings settings{};
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        return settings;
    }
    const std::wstring path = directory + L"ShenLong.ini";
    const auto material_gate_keys = spatch::graphics::ini::SettingKeys(
        L"MaterialScattering", L"WaterScattering", L"water_scattering");

    const bool master_enabled = spatch::graphics::ini::ReadBool(
        path, spatch::graphics::ini::kMasterEnabledKeys, false);
    const bool material_enabled = spatch::graphics::ini::ReadBool(
        path, material_gate_keys, true);
    settings.enabled = master_enabled && material_enabled;
    return settings;
}

std::size_t VariantIndex(WaterVariant variant) noexcept {
    switch (variant) {
    case WaterVariant::main:
        return 0;
    case WaterVariant::simple:
        return 1;
    case WaterVariant::blend:
        return 2;
    default:
        return kShaderIdentities.size();
    }
}

std::uint32_t VariantBit(WaterVariant variant) noexcept {
    const std::size_t index = VariantIndex(variant);
    return index < kShaderIdentities.size()
        ? 1u << static_cast<unsigned>(index)
        : 0u;
}

std::optional<WaterVariant> DecodeWaterShaderTag(
    std::uint32_t encoded) noexcept {
    if (encoded < static_cast<std::uint32_t>(WaterVariant::main) ||
        encoded > static_cast<std::uint32_t>(WaterVariant::blend)) {
        return std::nullopt;
    }
    return static_cast<WaterVariant>(encoded);
}

bool MatchesDxbcIdentity(
    const void* code,
    std::size_t code_size,
    std::size_t expected_size,
    const std::array<std::uint8_t, 16>& expected_checksum,
    std::uint32_t expected_crc32) noexcept {
    if (code == nullptr || code_size != expected_size || code_size < 20) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    return std::memcmp(bytes, "DXBC", 4) == 0 &&
        std::memcmp(
            bytes + 4,
            expected_checksum.data(),
            expected_checksum.size()) == 0 &&
        compute_crc32(bytes, code_size) == expected_crc32;
}

std::optional<WaterVariant> MatchWaterShaderIdentity(
    const void* code,
    std::size_t code_size) noexcept {
    for (const ShaderIdentity& identity : kShaderIdentities) {
        if (MatchesDxbcIdentity(
                code,
                code_size,
                identity.native_bytecode_size,
                identity.native_dxbc_checksum,
                identity.native_crc32)) {
            return identity.variant;
        }
    }
    return std::nullopt;
}

#if defined(SPATCH_WATER_DEVELOPMENT)
struct EnvironmentMapSrvDetails {
    UINT most_detailed_mip = 0;
    UINT mip_levels = 0;
    UINT first_array_slice = 0;
    UINT array_size = 0;
    UINT first_cube_face = 0;
    UINT cube_count = 0;
};

EnvironmentMapSrvDetails GetEnvironmentMapSrvDetails(
    const D3D11_SHADER_RESOURCE_VIEW_DESC& description) noexcept {
    EnvironmentMapSrvDetails details{};
    switch (description.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        details.most_detailed_mip = description.Texture2D.MostDetailedMip;
        details.mip_levels = description.Texture2D.MipLevels;
        details.array_size = 1;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        details.most_detailed_mip =
            description.Texture2DArray.MostDetailedMip;
        details.mip_levels = description.Texture2DArray.MipLevels;
        details.first_array_slice =
            description.Texture2DArray.FirstArraySlice;
        details.array_size = description.Texture2DArray.ArraySize;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        details.array_size = 1;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        details.first_array_slice =
            description.Texture2DMSArray.FirstArraySlice;
        details.array_size = description.Texture2DMSArray.ArraySize;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        details.most_detailed_mip =
            description.TextureCube.MostDetailedMip;
        details.mip_levels = description.TextureCube.MipLevels;
        details.array_size = 6;
        details.cube_count = 1;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
        details.most_detailed_mip =
            description.TextureCubeArray.MostDetailedMip;
        details.mip_levels = description.TextureCubeArray.MipLevels;
        details.first_array_slice =
            description.TextureCubeArray.First2DArrayFace;
        details.array_size = description.TextureCubeArray.NumCubes * 6u;
        details.first_cube_face =
            description.TextureCubeArray.First2DArrayFace;
        details.cube_count = description.TextureCubeArray.NumCubes;
        break;
    default:
        break;
    }
    return details;
}

void ReportEnvironmentMapTelemetryFailure(
    DeviceData& data,
    std::size_t variant_index,
    const char* reason) noexcept {
    if (!data.envmap_telemetry_failure_written.exchange(
            true, std::memory_order_relaxed)) {
        Log(reshade::log::level::warning,
            "[ShenLong-Water] Development texEnvMap trace failed "
            "variant=%s native_crc=0x%08X reason=%s; the telemetry attempt "
            "is not retried and rendering is unchanged.",
            variant_index < kShaderIdentities.size()
                ? kShaderIdentities[variant_index].display_name
                : "unknown",
            variant_index < kShaderIdentities.size()
                ? kShaderIdentities[variant_index].native_crc32
                : 0u,
            reason != nullptr ? reason : "unknown");
    }
}

void CaptureEnvironmentMapTelemetry(
    DeviceData& data,
    ID3D11DeviceContext* context,
    WaterVariant variant) noexcept {
    const std::size_t variant_index = VariantIndex(variant);
    const std::uint32_t variant_bit = VariantBit(variant);
    if (variant_index >= kShaderIdentities.size() || variant_bit == 0u) {
        return;
    }
    if ((data.envmap_telemetry_capture_mask.fetch_or(
            variant_bit, std::memory_order_relaxed) & variant_bit) != 0u) {
        return;
    }
    if (context == nullptr) {
        ReportEnvironmentMapTelemetryFailure(
            data, variant_index, "missing-immediate-context");
        return;
    }

    ID3D11ShaderResourceView* raw_srv = nullptr;
    context->PSGetShaderResources(1, 1, &raw_srv);
    ComPtr<ID3D11ShaderResourceView> srv;
    srv.Attach(raw_srv);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_description{};
    if (srv) {
        srv->GetDesc(&srv_description);
    }
    const EnvironmentMapSrvDetails srv_details =
        GetEnvironmentMapSrvDetails(srv_description);

    ComPtr<ID3D11Resource> resource;
    if (srv) {
        srv->GetResource(resource.GetAddressOf());
    }
    D3D11_RESOURCE_DIMENSION resource_dimension =
        D3D11_RESOURCE_DIMENSION_UNKNOWN;
    HRESULT texture2d_query_result = E_POINTER;
    ComPtr<ID3D11Texture2D> texture2d;
    D3D11_TEXTURE2D_DESC texture2d_description{};
    if (resource) {
        resource->GetType(&resource_dimension);
        texture2d_query_result = resource->QueryInterface(
            IID_PPV_ARGS(texture2d.GetAddressOf()));
        if (SUCCEEDED(texture2d_query_result) && texture2d) {
            texture2d->GetDesc(&texture2d_description);
        }
    }
    const bool texture2d_query_succeeded =
        SUCCEEDED(texture2d_query_result) && texture2d;

    ID3D11SamplerState* raw_sampler = nullptr;
    context->PSGetSamplers(1, 1, &raw_sampler);
    ComPtr<ID3D11SamplerState> sampler;
    sampler.Attach(raw_sampler);
    D3D11_SAMPLER_DESC sampler_description{};
    if (sampler) {
        sampler->GetDesc(&sampler_description);
    }

    Log(reshade::log::level::info,
        "[ShenLong-Water] Development texEnvMap trace variant=%s "
        "native_crc=0x%08X resource_present=%d dimension=%u "
        "texture2d_query=0x%08lX query_success=%d "
        "tex2d={format=%u width=%u height=%u array=%u mips=%u samples=%u "
        "usage=%u bind=0x%X cpu=0x%X misc=0x%X} "
        "srv={present=%d format=%u dimension=%u most_mip=%u mips=%u "
        "array_first=%u array_size=%u cube_first_face=%u cubes=%u} "
        "sampler={present=%d filter=0x%X aniso=%u address=[%u,%u,%u] "
        "bias=%.6g lod=[%.6g,%.6g]}.",
        kShaderIdentities[variant_index].display_name,
        kShaderIdentities[variant_index].native_crc32,
        resource ? 1 : 0,
        static_cast<unsigned int>(resource_dimension),
        static_cast<unsigned long>(texture2d_query_result),
        texture2d_query_succeeded ? 1 : 0,
        static_cast<unsigned int>(texture2d_description.Format),
        texture2d_description.Width,
        texture2d_description.Height,
        texture2d_description.ArraySize,
        texture2d_description.MipLevels,
        texture2d_description.SampleDesc.Count,
        static_cast<unsigned int>(texture2d_description.Usage),
        texture2d_description.BindFlags,
        texture2d_description.CPUAccessFlags,
        texture2d_description.MiscFlags,
        srv ? 1 : 0,
        static_cast<unsigned int>(srv_description.Format),
        static_cast<unsigned int>(srv_description.ViewDimension),
        srv_details.most_detailed_mip,
        srv_details.mip_levels,
        srv_details.first_array_slice,
        srv_details.array_size,
        srv_details.first_cube_face,
        srv_details.cube_count,
        sampler ? 1 : 0,
        static_cast<unsigned int>(sampler_description.Filter),
        sampler_description.MaxAnisotropy,
        static_cast<unsigned int>(sampler_description.AddressU),
        static_cast<unsigned int>(sampler_description.AddressV),
        static_cast<unsigned int>(sampler_description.AddressW),
        sampler_description.MipLODBias,
        sampler_description.MinLOD,
        sampler_description.MaxLOD);

    const char* failure_reason = nullptr;
    if (!srv) {
        failure_reason = "missing-texEnvMap-t1";
    } else if (!resource) {
        failure_reason = "texEnvMap-resource-unavailable";
    } else if (!texture2d_query_succeeded) {
        failure_reason = "texEnvMap-resource-is-not-texture2d";
    } else if (!sampler) {
        failure_reason = "missing-texEnvMap-s1";
    }
    if (failure_reason != nullptr) {
        ReportEnvironmentMapTelemetryFailure(
            data, variant_index, failure_reason);
    }
}
#endif

#if defined(SPATCH_WATER_DEVELOPMENT)
bool CompilePixelShaderBytecode(
    const std::wstring& path,
    ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const HRESULT result = D3DCompileFromFile(
        path.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main",
        "ps_4_0",
        flags,
        0,
        bytecode.ReleaseAndGetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(result) || !bytecode) {
        const char* detail = errors && errors->GetBufferPointer()
            ? static_cast<const char*>(errors->GetBufferPointer())
            : "no compiler diagnostics";
        Log(reshade::log::level::error,
            "[ShenLong-Water] shader compile failed for %ls: %s",
            path.c_str(),
            detail);
        return false;
    }
    return true;
}
#endif

#if defined(SPATCH_WATER_DEVELOPMENT)
bool CreatePixelShaderFromDevelopmentSource(
    ID3D11Device* device,
    const std::wstring& source_path,
    ComPtr<ID3D11PixelShader>& output,
    bool& used_source_fallback) {
    used_source_fallback = true;
    ComPtr<ID3DBlob> source_bytecode;
    if (!CompilePixelShaderBytecode(source_path, source_bytecode)) {
        return false;
    }

    output.Reset();
    const HRESULT create_result = device->CreatePixelShader(
        source_bytecode->GetBufferPointer(),
        source_bytecode->GetBufferSize(),
        nullptr,
        output.ReleaseAndGetAddressOf());
    if (FAILED(create_result) || !output) {
        Log(reshade::log::level::error,
            "[ShenLong-Water] CreatePixelShader failed for Development source "
            "%ls (HRESULT=0x%08lX); native water retained.",
            source_path.c_str(),
            static_cast<unsigned long>(create_result));
        return false;
    }
    return true;
}
#endif

bool LoadPixelShader(
    ID3D11Device* device,
    const ShaderIdentity& identity,
    const std::wstring& cache_path,
    const std::wstring& source_path,
    ComPtr<ID3D11PixelShader>& output,
    bool& used_source_fallback) {
    ComPtr<ID3DBlob> bytecode;
    const HRESULT load_result = D3DReadFileToBlob(
        cache_path.c_str(), bytecode.ReleaseAndGetAddressOf());
    if (FAILED(load_result) || !bytecode) {
#if defined(SPATCH_WATER_DEVELOPMENT)
        Log(reshade::log::level::warning,
            "[ShenLong-Water] precompiled shader unavailable (%ls, "
            "HRESULT=0x%08lX); using the Development source fallback.",
            cache_path.c_str(),
            static_cast<unsigned long>(load_result));
        return CreatePixelShaderFromDevelopmentSource(
            device, source_path, output, used_source_fallback);
#else
        static_cast<void>(source_path);
        static_cast<void>(used_source_fallback);
        Log(reshade::log::level::error,
            "[ShenLong-Water] required precompiled shader is missing or invalid "
            "(%ls, HRESULT=0x%08lX); native water retained.",
            cache_path.c_str(),
            static_cast<unsigned long>(load_result));
        return false;
#endif
    }

    const void* const cache_code = bytecode->GetBufferPointer();
    const std::size_t cache_size = bytecode->GetBufferSize();
    if (!MatchesDxbcIdentity(
            cache_code,
            cache_size,
            identity.cache_bytecode_size,
            identity.cache_dxbc_checksum,
            identity.cache_crc32)) {
#if defined(SPATCH_WATER_DEVELOPMENT)
        Log(reshade::log::level::warning,
            "[ShenLong-Water] precompiled shader identity mismatch for %ls "
            "(bytes=%zu, expected=%zu); using the Development source "
            "fallback.",
            cache_path.c_str(),
            cache_size,
            identity.cache_bytecode_size);
        return CreatePixelShaderFromDevelopmentSource(
            device, source_path, output, used_source_fallback);
#else
        Log(reshade::log::level::error,
            "[ShenLong-Water] precompiled shader identity mismatch for %ls "
            "(bytes=%zu, expected=%zu); native water retained.",
            cache_path.c_str(),
            cache_size,
            identity.cache_bytecode_size);
        return false;
#endif
    }

    const HRESULT create_result = device->CreatePixelShader(
        cache_code,
        cache_size,
        nullptr,
        output.ReleaseAndGetAddressOf());
    if (FAILED(create_result) || !output) {
#if defined(SPATCH_WATER_DEVELOPMENT)
        Log(reshade::log::level::warning,
            "[ShenLong-Water] CreatePixelShader failed for precompiled %ls "
            "(HRESULT=0x%08lX); using the Development source fallback.",
            cache_path.c_str(),
            static_cast<unsigned long>(create_result));
        return CreatePixelShaderFromDevelopmentSource(
            device, source_path, output, used_source_fallback);
#else
        Log(reshade::log::level::error,
            "[ShenLong-Water] CreatePixelShader failed for %ls "
            "(HRESULT=0x%08lX); native water retained.",
            cache_path.c_str(),
            static_cast<unsigned long>(create_result));
        return false;
#endif
    }
    return true;
}

bool CreateReplacementResources(ID3D11Device* device, DeviceData& data) {
    const std::wstring directory = ModuleDirectory();
    if (device == nullptr || directory.empty()) {
        return false;
    }
    const std::wstring cache_root =
        directory + L"ShenLong\\ShaderCache\\v1\\Water\\";
    const std::wstring source_root =
        directory + L"ShenLong\\Shaders\\Water\\";
    std::array<ComPtr<ID3D11PixelShader>, 3> shaders;
    bool used_source_fallback = false;
    for (const ShaderIdentity& identity : kShaderIdentities) {
        const std::size_t index = VariantIndex(identity.variant);
        if (index >= shaders.size() ||
            !LoadPixelShader(
                device,
                identity,
                cache_root + identity.cache_name,
                source_root + identity.source_name,
                shaders[index],
                used_source_fallback)) {
            return false;
        }
    }

    const WaterConstants constants{
        1.0f,
        0.0f,
        0.0f,
        0.0f,
    };
    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(constants);
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial_data{};
    initial_data.pSysMem = &constants;
    ComPtr<ID3D11Buffer> settings_buffer;
    const HRESULT buffer_result = device->CreateBuffer(
        &buffer_desc, &initial_data, settings_buffer.GetAddressOf());
    if (FAILED(buffer_result) || !settings_buffer) {
        Log(reshade::log::level::error,
            "[ShenLong-Water] settings buffer creation failed "
            "(HRESULT=0x%08lX); native water retained.",
            static_cast<unsigned long>(buffer_result));
        return false;
    }

    data.replacement_shaders = std::move(shaders);
    data.settings_buffer = std::move(settings_buffer);
    Log(reshade::log::level::info,
        used_source_fallback
            ? "[ShenLong-Water] all three water shaders initialized; at least "
              "one used the Development source fallback."
            : "[ShenLong-Water] precompiled shader cache v1 loaded for all "
              "three exact water variants.");
    return true;
}

bool ValidateConstantBuffer(
    ID3D11DeviceContext* context,
    UINT slot,
    UINT minimum_size) noexcept {
    ComPtr<ID3D11Buffer> buffer;
    context->PSGetConstantBuffers(slot, 1, buffer.GetAddressOf());
    if (!buffer) {
        return false;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    return desc.ByteWidth >= minimum_size &&
        (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

bool ValidateShaderResources(
    ID3D11DeviceContext* context,
    UINT required_count) noexcept {
    std::array<ID3D11ShaderResourceView*, 8> resources{};
    if (required_count > resources.size()) {
        return false;
    }
    context->PSGetShaderResources(0, required_count, resources.data());
    bool valid = true;
    for (UINT index = 0; index < required_count; ++index) {
        valid = valid && resources[index] != nullptr;
        if (resources[index] != nullptr) {
            resources[index]->Release();
        }
    }
    return valid;
}

bool ValidateBindings(
    ID3D11DeviceContext* context,
    WaterVariant variant) noexcept {
    // Validate the ranges actually declared by the stock DXBC, not the full
    // source-level structs reported by reflection. Sleeping Dogs is allowed to
    // bind compact buffers which cover only these statically accessed vectors.
    if (context == nullptr ||
        !ValidateConstantBuffer(context, 0, 176) ||
        !ValidateConstantBuffer(
            context,
            1,
            variant == WaterVariant::simple ? 80u : 96u) ||
        !ValidateConstantBuffer(context, 2, 48) ||
        !ValidateConstantBuffer(context, 3, 16)) {
        return false;
    }
    if (variant == WaterVariant::blend) {
        return ValidateConstantBuffer(context, 4, 64) &&
            ValidateShaderResources(context, 7);
    }
    if (variant == WaterVariant::main || variant == WaterVariant::simple) {
        return ValidateConstantBuffer(context, 4, 16) &&
            ValidateConstantBuffer(context, 5, 64) &&
            ValidateShaderResources(context, 8);
    }
    return false;
}

std::optional<WaterVariant> ReadBoundWaterVariant(
    ID3D11DeviceContext* context) noexcept {
    if (context == nullptr) {
        return std::nullopt;
    }
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
    if (!shader) {
        return std::nullopt;
    }
    std::uint32_t encoded = 0;
    UINT size = sizeof(encoded);
    if (FAILED(shader->GetPrivateData(kWaterShaderTag, &size, &encoded)) ||
        size != sizeof(encoded)) {
        return std::nullopt;
    }
    return DecodeWaterShaderTag(encoded);
}

bool ReplayDraw(
    reshade::api::command_list* command_list,
    const DrawCall& draw_call) noexcept {
    if (draw_call.count == 0 || draw_call.instance_count == 0 ||
        g_replaying_draw || command_list == nullptr) {
        return false;
    }
    DeviceData* data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (data == nullptr || !data->ready || !data->settings.enabled) {
        return false;
    }
    data->draw_callbacks.fetch_add(1, std::memory_order_relaxed);

    auto* context = NativePointer<ID3D11DeviceContext>(
        command_list->get_native());
    // The proven game path uses the immediate context. Deferred command lists
    // are deliberately left native rather than trusting potentially stale
    // shader state from an event callback.
    if (context == nullptr ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }
    const std::optional<WaterVariant> variant =
        ReadBoundWaterVariant(context);
    if (!variant) {
        return false;
    }
    const std::size_t index = VariantIndex(*variant);
    if (index >= data->replacement_shaders.size() ||
        !data->replacement_shaders[index] || !data->settings_buffer) {
        return false;
    }
    if (!ValidateBindings(context, *variant)) {
        data->validation_failures[index].fetch_add(1, std::memory_order_relaxed);
        bool expected = false;
        if (data->validation_warning_written.compare_exchange_strong(
                expected, true)) {
            Log(reshade::log::level::warning,
                "[ShenLong-Water] exact water variant %s failed binding "
                "validation; native draws are retained.",
                kShaderIdentities[index].display_name);
        }
        return false;
    }

#if defined(SPATCH_WATER_DEVELOPMENT)
    CaptureEnvironmentMapTelemetry(*data, context, *variant);
#endif

    ComPtr<ID3D11PixelShader> original_shader;
    ComPtr<ID3D11Buffer> original_settings;
    context->PSGetShader(original_shader.GetAddressOf(), nullptr, nullptr);
    context->PSGetConstantBuffers(
        kSettingsBufferSlot, 1, original_settings.GetAddressOf());
    if (!original_shader) {
        return false;
    }

    ID3D11Buffer* settings_buffer = data->settings_buffer.Get();
    context->PSSetConstantBuffers(
        kSettingsBufferSlot, 1, &settings_buffer);
    context->PSSetShader(
        data->replacement_shaders[index].Get(), nullptr, 0);

    g_replaying_draw = true;
    if (draw_call.indexed) {
        if (draw_call.instance_count == 1 && draw_call.first_instance == 0) {
            context->DrawIndexed(
                draw_call.count, draw_call.first, draw_call.vertex_offset);
        } else {
            context->DrawIndexedInstanced(
                draw_call.count,
                draw_call.instance_count,
                draw_call.first,
                draw_call.vertex_offset,
                draw_call.first_instance);
        }
    } else if (draw_call.instance_count == 1 &&
               draw_call.first_instance == 0) {
        context->Draw(draw_call.count, draw_call.first);
    } else {
        context->DrawInstanced(
            draw_call.count,
            draw_call.instance_count,
            draw_call.first,
            draw_call.first_instance);
    }
    g_replaying_draw = false;

    context->PSSetShader(original_shader.Get(), nullptr, 0);
    ID3D11Buffer* restored_settings = original_settings.Get();
    context->PSSetConstantBuffers(
        kSettingsBufferSlot, 1, &restored_settings);
    data->replacement_draws[index].fetch_add(1, std::memory_order_relaxed);
    data->replacement_variants.fetch_or(
        VariantBit(*variant), std::memory_order_relaxed);
    return true;
}

void OnInitDevice(reshade::api::device* device) {
    if (device == nullptr) {
        return;
    }
    DeviceData* data =
        spatch::graphics::detail::CreatePrivateData<DeviceData>(device);
    if (data == nullptr) {
        Log(reshade::log::level::error,
            "[ShenLong-Water] device state allocation failed; native water retained.");
        return;
    }
    data->settings = ReadSettings();
    if (!data->settings.enabled) {
        Log(reshade::log::level::info,
            "[ShenLong-Water] disabled by ShenLong.ini; native water retained.");
        return;
    }
    if (device->get_api() != reshade::api::device_api::d3d11) {
        Log(reshade::log::level::error,
            "[ShenLong-Water] Direct3D 11 is required; native water retained.");
        return;
    }
    auto* native_device = NativePointer<ID3D11Device>(device->get_native());
    data->ready = native_device != nullptr &&
        CreateReplacementResources(native_device, *data);
    Log(data->ready
            ? reshade::log::level::info
            : reshade::log::level::error,
        "[ShenLong-Water] enabled=%d ready=%d isotropic_strength=100%%.",
        data->settings.enabled ? 1 : 0,
        data->ready ? 1 : 0);
}

void OnDestroyDevice(reshade::api::device* device) {
    device->destroy_private_data<DeviceData>();
}

void OnInitPipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline) {
    if (device == nullptr ||
        device->get_api() != reshade::api::device_api::d3d11 ||
        subobjects == nullptr || pipeline.handle == 0) {
        return;
    }
    DeviceData* data = device->get_private_data<DeviceData>();
    if (data == nullptr) {
        return;
    }
    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const reshade::api::pipeline_subobject& subobject = subobjects[index];
        if (subobject.type != reshade::api::pipeline_subobject_type::pixel_shader ||
            subobject.count == 0 || subobject.data == nullptr) {
            continue;
        }
        const reshade::api::shader_desc& shader =
            static_cast<const reshade::api::shader_desc*>(subobject.data)[0];
        const std::optional<WaterVariant> variant =
            MatchWaterShaderIdentity(shader.code, shader.code_size);
        if (!variant) {
            return;
        }

        auto* native_shader = NativePointer<ID3D11PixelShader>(pipeline.handle);
        const std::uint32_t encoded = static_cast<std::uint32_t>(*variant);
        const HRESULT tag_result = native_shader->SetPrivateData(
            kWaterShaderTag, sizeof(encoded), &encoded);
        if (SUCCEEDED(tag_result)) {
            data->discovered_variants.fetch_or(
                VariantBit(*variant), std::memory_order_relaxed);
        } else {
            bool expected = false;
            if (data->registration_warning_written.compare_exchange_strong(
                    expected, true)) {
                Log(reshade::log::level::warning,
                    "[ShenLong-Water] exact shader identity was found but could "
                    "not be tagged; affected draws remain native.");
            }
        }
        return;
    }
}

bool OnDraw(
    reshade::api::command_list* command_list,
    std::uint32_t vertex_count,
    std::uint32_t instance_count,
    std::uint32_t first_vertex,
    std::uint32_t first_instance) {
    return ReplayDraw(command_list, DrawCall{
        false,
        vertex_count,
        instance_count,
        first_vertex,
        0,
        first_instance,
    });
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) {
    return ReplayDraw(command_list, DrawCall{
        true,
        index_count,
        instance_count,
        first_index,
        vertex_offset,
        first_instance,
    });
}

void OnPresent(
    reshade::api::command_queue* queue,
    reshade::api::swapchain*,
    const reshade::api::rect*,
    const reshade::api::rect*,
    std::uint32_t,
    const reshade::api::rect*) {
    DeviceData* data = queue != nullptr
        ? queue->get_device()->get_private_data<DeviceData>()
        : nullptr;
    if (data == nullptr || !data->ready || !data->settings.enabled) {
        return;
    }
    const std::uint64_t presents =
        data->presents.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint32_t replaced =
        data->replacement_variants.load(std::memory_order_relaxed);
    if (replaced != 0) {
        bool expected = false;
        if (data->success_written.compare_exchange_strong(expected, true)) {
            Log(reshade::log::level::info,
                "[ShenLong-Water] bounded single scattering confirmed on an "
                "exact native water draw (variants=0x%X).",
                replaced);
        }
    }
    if (presents == 1800) {
        bool expected = false;
        if (data->coverage_written.compare_exchange_strong(expected, true)) {
            const std::uint32_t discovered =
                data->discovered_variants.load(std::memory_order_relaxed);
            Log(reshade::log::level::info,
                "[ShenLong-Water] coverage discovered=0x%X/%X replaced=0x%X/%X "
                "draws=[%llu,%llu,%llu] validation_failures=[%llu,%llu,%llu] "
                "callbacks=%llu.",
                discovered,
                kAllWaterVariants,
                replaced,
                kAllWaterVariants,
                static_cast<unsigned long long>(
                    data->replacement_draws[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data->replacement_draws[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data->replacement_draws[2].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data->validation_failures[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data->validation_failures[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data->validation_failures[2].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    data->draw_callbacks.load(std::memory_order_relaxed)));
        }
    }
}

}  // namespace

namespace spatch::graphics::water {

void Attach(HMODULE module) {
    g_module = module;
    reshade::register_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::register_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::register_event<reshade::addon_event::draw>(
        GuardedCallback<OnDraw>::Invoke);
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
    reshade::unregister_event<reshade::addon_event::draw>(
        GuardedCallback<OnDraw>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(
        GuardedCallback<OnInitPipeline>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    g_module = nullptr;
}

}  // namespace spatch::graphics::water

#if !defined(SPATCH_GRAPHICS_UNIFIED)
extern "C" __declspec(dllexport) const char* NAME = "SPatch Water Scattering";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Bounded water single scattering for Sleeping Dogs: Definitive Edition";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) {
            return FALSE;
        }
        spatch::graphics::water::Attach(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        spatch::graphics::water::Detach();
        reshade::unregister_addon(module);
    }
    return TRUE;
}
#endif
