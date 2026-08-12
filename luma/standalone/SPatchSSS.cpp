// SPatch Jimenez separable subsurface-scattering add-on for Sleeping Dogs: DE.
//
// The game exposes no public material mask, so two guarded native submit hooks
// associate exact index-buffer ranges with validated material-profile UIDs. The
// D3D11 add-on writes those ranges into a private depth/stencil mask and filters
// the HDR direct-light buffer immediately before the stock final composition.

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <examples/utils/crc32_hash.hpp>
#include <MinHook.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SPatchGraphicsComponents.hpp"
#include "SPatchIni.hpp"
#include "SPatchReShadeCallbackSafety.hpp"
#include "ShenLongNative.hpp"

using Microsoft::WRL::ComPtr;

namespace {

#include "SkinMaterialUids.inl"
#include "CharacterMaterialProfiles.inl"
#include "EyeMaterialProfiles.inl"
#include "FoliageMaterialUids.inl"

enum class MaterialProfile : std::uint32_t {
    none = 0,
    skin = 1,
    eye = 2,
    hair = 3,
    teeth = 4,
    foliage = 5,
};

template <std::size_t Size>
constexpr bool IsStrictlySortedUnique(
    const std::array<std::uint32_t, Size>& values) noexcept {
    for (std::size_t index = 1; index < values.size(); ++index) {
        if (values[index - 1] >= values[index]) {
            return false;
        }
    }
    return true;
}

static_assert(IsStrictlySortedUnique(kSkinMaterialUids));
static_assert(IsStrictlySortedUnique(kEyeMaterialUids));
static_assert(IsStrictlySortedUnique(kHairMaterialUids));
static_assert(IsStrictlySortedUnique(kTeethMaterialUids));
static_assert(IsStrictlySortedUnique(kFoliageMaterialUids));

constexpr bool IsKnownSkinMaterial(std::uint32_t material_uid) noexcept {
    return std::binary_search(
        kSkinMaterialUids.begin(), kSkinMaterialUids.end(), material_uid);
}

template <std::size_t Size>
constexpr bool ContainsMaterial(
    const std::array<std::uint32_t, Size>& materials,
    std::uint32_t material_uid) noexcept {
    return std::binary_search(materials.begin(), materials.end(), material_uid);
}

constexpr MaterialProfile ClassifyMaterial(
    std::uint32_t material_uid) noexcept {
    if (IsKnownSkinMaterial(material_uid)) {
        return MaterialProfile::skin;
    }
    if (ContainsMaterial(kEyeMaterialUids, material_uid)) {
        return MaterialProfile::eye;
    }
    if (ContainsMaterial(kHairMaterialUids, material_uid)) {
        return MaterialProfile::hair;
    }
    if (ContainsMaterial(kTeethMaterialUids, material_uid)) {
        return MaterialProfile::teeth;
    }
    if (ContainsMaterial(kFoliageMaterialUids, material_uid)) {
        return MaterialProfile::foliage;
    }
    return MaterialProfile::none;
}

// Keep the researched boundary explicit. These representative IDs cover Wei,
// an NPC, and nearby non-skin materials that must never enter the blur.
static_assert(IsKnownSkinMaterial(0xB1FF008Cu));  // Wei head
static_assert(IsKnownSkinMaterial(0x34F4A5FEu));  // Wei arms
static_assert(IsKnownSkinMaterial(0x38FFB06Du));  // NPC skin
static_assert(!IsKnownSkinMaterial(0x528474ADu)); // eyes
static_assert(!IsKnownSkinMaterial(0x1B28F0BBu)); // eyelashes
static_assert(!IsKnownSkinMaterial(0x65B1684Fu)); // hair

constexpr std::uint32_t kFinalCompositionPixelShaderHash = 0x1964CD11;
constexpr std::uint32_t kSkinnedGBufferVertexShaderHash = 0x93224036;
constexpr std::uint32_t kEyeGBufferPixelShaderHash = 0xF64A98D5;
constexpr std::uint32_t kEyeGBufferVertexShaderHash = 0x38221CAD;
constexpr std::uint32_t kHairGBufferPixelShaderHash = 0x0A6EDB3E;
constexpr std::uint32_t kHairGBufferVertexShaderHash = 0x3C180790;
constexpr std::uint32_t kFoliageGBufferPixelShaderHash = 0x537E7246;
constexpr std::uint32_t kFoliageGBufferVertexShaderHash = 0x1200CCEF;
constexpr std::uint32_t kFoliageGBufferWnVertexShaderHash = 0xE5541930;
constexpr float kJimenezBaseRadiusMetres = 0.012f;
constexpr std::uint64_t kRangeLifetimeFrames = 600;
constexpr std::size_t kMaterialProfileCount = 6;

struct ShaderIdentity {
    std::uint32_t crc32;
    std::size_t bytecode_size;
    std::array<std::uint8_t, 16> dxbc_checksum;
};

constexpr std::array<ShaderIdentity, 4> kTrackedPixelShaders{{
    {kFinalCompositionPixelShaderHash, 4976,
     {0xDB, 0xC7, 0x39, 0x98, 0x36, 0x66, 0xB5, 0x32,
      0x5A, 0xA8, 0x26, 0xFE, 0x29, 0x52, 0x95, 0x10}},
    {kEyeGBufferPixelShaderHash, 4864,
     {0xC9, 0x46, 0x8A, 0x2C, 0xF6, 0x7D, 0x79, 0xC3,
      0xCA, 0x3C, 0xEA, 0x8D, 0x89, 0x9F, 0xB6, 0xE9}},
    {kHairGBufferPixelShaderHash, 2160,
     {0x07, 0xF6, 0x69, 0x9A, 0x27, 0x57, 0xD4, 0x67,
      0x52, 0x6E, 0xC5, 0x19, 0x20, 0xE4, 0xE8, 0xA5}},
    {kFoliageGBufferPixelShaderHash, 2328,
     {0x24, 0x83, 0xCC, 0xEC, 0x38, 0x55, 0x19, 0x03,
      0xB1, 0x95, 0x62, 0xE4, 0xE1, 0xC0, 0x12, 0x79}},
}};

constexpr std::array<ShaderIdentity, 5> kTrackedVertexShaders{{
    {kSkinnedGBufferVertexShaderHash, 2236,
     {0xE4, 0x94, 0x5C, 0xAE, 0xBC, 0x43, 0xC6, 0xA7,
      0x9D, 0xEA, 0xAC, 0xCE, 0x02, 0x8A, 0x78, 0xCB}},
    {kEyeGBufferVertexShaderHash, 2744,
     {0x79, 0x6D, 0xBD, 0xCC, 0x38, 0x79, 0x58, 0xC6,
      0x32, 0x58, 0x79, 0xF0, 0x68, 0x6A, 0xEB, 0x1E}},
    {kHairGBufferVertexShaderHash, 1712,
     {0xE8, 0x3E, 0xA1, 0xD5, 0x6B, 0xD3, 0x90, 0xE5,
      0x4D, 0x20, 0x3B, 0xF1, 0xE1, 0xF8, 0xF0, 0x9A}},
    {kFoliageGBufferVertexShaderHash, 1264,
     {0x16, 0xDB, 0xA4, 0x41, 0x41, 0x8D, 0x0C, 0x73,
      0x9C, 0x3F, 0x41, 0x26, 0xB8, 0x55, 0x3F, 0xEF}},
    {kFoliageGBufferWnVertexShaderHash, 5192,
     {0xD8, 0x38, 0x63, 0x43, 0xAA, 0x0F, 0x7F, 0xC7,
      0xEF, 0x2A, 0x2D, 0xC1, 0x51, 0x91, 0xBF, 0x3E}},
}};

constexpr bool IsTrackedGBufferVertexShader(std::uint32_t hash) noexcept {
    return hash == kSkinnedGBufferVertexShaderHash ||
        hash == kEyeGBufferVertexShaderHash ||
        hash == kHairGBufferVertexShaderHash ||
        hash == kFoliageGBufferVertexShaderHash ||
        hash == kFoliageGBufferWnVertexShaderHash;
}

constexpr GUID kPixelShaderHashTag = {
    0x2413b6a4, 0xda2a, 0x4fde, {0xa5, 0xd4, 0x32, 0x25, 0x97, 0xe9, 0x48, 0xe3}};
constexpr GUID kVertexShaderHashTag = {
    0xa2df71e2, 0xb49e, 0x4a8d, {0x90, 0x57, 0x35, 0xa0, 0xa4, 0x85, 0x5e, 0xab}};

// Exact, relocation-free prefixes. The add-on scans executable sections and
// requires one unique match for each function, so unknown executable layouts
// fail closed without installing either hook.
constexpr std::array<std::uint8_t, 24> kRenderSubmitSignature{
    0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41,
    0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
    0x24, 0xD9, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00};
constexpr std::array<std::uint8_t, 24> kAlternateSubmitSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00, 0x49};

struct Settings {
    bool enabled = true;
    bool skin_enabled = true;
    bool eye_enabled = true;
    bool hair_enabled = true;
    bool teeth_enabled = true;
    bool foliage_enabled = true;
    int quality = 2;
    float strength = 0.75f;
    float radius_scale = 1.0f;
};

enum class SssRunResult : std::uint8_t {
    skipped,
    applied,
    input_unavailable,
};

struct alignas(16) SssConstants {
    float inverse_resolution[2]{};
    float radius = kJimenezBaseRadiusMetres;
    float strength = 1.0f;
    float direction[2]{};
    float specular_scale = 1.0f;
    float debug_view = 0.0f;
    std::uint32_t material_profile = 0;
    float profile_anisotropy = 1.0f;
    float profile_mask_scale = 1.0f;
    float profile_padding = 0.0f;
};

static_assert(sizeof(SssConstants) == 48);

struct alignas(16) CaptureConstants {
    float eye_center[2]{0.5f, 0.5f};
    float eye_radius[2]{0.1f, 0.1f};
    float eye_iris_inner = 0.38f;
    float eye_iris_outer = 0.48f;
    float padding[2]{};
};

static_assert(sizeof(CaptureConstants) == 32);

#if defined(SPATCH_SSS_DEVELOPMENT)
struct GpuTimingSlot {
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> start;
    ComPtr<ID3D11Query> end;
    bool pending = false;
};
#endif

struct __declspec(uuid("5D6CC0A7-66FC-444C-83DF-A9E1CD40547E")) DeviceData {
    Settings settings;
    bool ready = false;
    bool runtime_registered = false;
    bool logged_active = false;
    bool logged_input_failure = false;
    std::uint32_t consecutive_input_failures = 0;
    ID3D11Device* native_device = nullptr;

    ComPtr<ID3D11VertexShader> fullscreen_vertex_shader;
    ComPtr<ID3D11PixelShader> horizontal_pixel_shader;
    ComPtr<ID3D11PixelShader> vertical_pixel_shader;
    ComPtr<ID3D11PixelShader> eye_mask_pixel_shader;
    ComPtr<ID3D11PixelShader> hair_capture_pixel_shader;
    ComPtr<ID3D11PixelShader> foliage_capture_pixel_shader;
    ComPtr<ID3D11PixelShader> foliage_transmission_pixel_shader;
    ComPtr<ID3D11SamplerState> linear_sampler;
    ComPtr<ID3D11BlendState> no_blend_state;
    ComPtr<ID3D11DepthStencilState> mask_write_state;
    ComPtr<ID3D11DepthStencilState> blur_stencil_state;
    ComPtr<ID3D11RasterizerState> fullscreen_rasterizer_state;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Buffer> capture_constants;

    ComPtr<ID3D11Texture2D> mask_depth;
    ComPtr<ID3D11DepthStencilView> mask_write_dsv;
    ComPtr<ID3D11DepthStencilView> mask_read_dsv;
    ComPtr<ID3D11ShaderResourceView> mask_depth_srv;
    ComPtr<ID3D11ShaderResourceView> mask_stencil_srv;
    ComPtr<ID3D11Texture2D> material_data;
    ComPtr<ID3D11RenderTargetView> material_data_rtv;
    ComPtr<ID3D11ShaderResourceView> material_data_srv;
    D3D11_TEXTURE2D_DESC mask_source_desc{};
    bool mask_source_desc_valid = false;
    D3D11_TEXTURE2D_DESC failed_mask_source_desc{};
    bool failed_mask_source_desc_valid = false;
    std::uint64_t mask_retry_after_frame = 0;
    bool logged_mask_allocation_failure = false;
    ComPtr<ID3D11Texture2D> scene_depth_resource;
    ComPtr<ID3D11ShaderResourceView> scene_depth_srv;
    std::uintptr_t failed_scene_depth_source = 0;
    std::uint64_t scene_depth_retry_after_frame = 0;
    bool logged_scene_depth_failure = false;

    ComPtr<ID3D11Texture2D> original_lighting;
    ComPtr<ID3D11ShaderResourceView> original_lighting_srv;
    ComPtr<ID3D11Texture2D> temporary_lighting;
    ComPtr<ID3D11RenderTargetView> temporary_lighting_rtv;
    ComPtr<ID3D11ShaderResourceView> temporary_lighting_srv;
    ComPtr<ID3D11Texture2D> output_lighting_resource;
    ComPtr<ID3D11RenderTargetView> output_lighting_rtv;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT lighting_format = DXGI_FORMAT_UNKNOWN;
    std::uintptr_t failed_lighting_source = 0;
    D3D11_TEXTURE2D_DESC failed_lighting_desc{};
    bool failed_lighting_desc_valid = false;
    std::uint64_t lighting_retry_after_frame = 0;
    bool logged_lighting_allocation_failure = false;
    std::uint64_t mask_frame = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t processed_frame = (std::numeric_limits<std::uint64_t>::max)();
    std::array<std::uint32_t, kMaterialProfileCount> mask_draw_counts{};
    std::uint64_t applied_frames = 0;
#if defined(SPATCH_SSS_DEVELOPMENT)
    std::array<GpuTimingSlot, 8> gpu_timing_slots{};
    std::array<double, 120> gpu_timing_samples{};
    std::size_t gpu_timing_cursor = 0;
    std::size_t gpu_timing_sample_count = 0;
    bool gpu_timing_ready = false;
    bool gpu_timing_logged = false;
#endif
};

struct __declspec(uuid("CBD227D8-37E5-4C25-BA11-C170B33FDCBC")) CommandListData {
    enum class GBufferBinding : std::uint8_t {
        unavailable,
        unvalidated,
        valid,
    };

    std::uint32_t pixel_shader_hash = 0;
    std::uint32_t vertex_shader_hash = 0;
    std::array<reshade::api::resource_view, 3> render_targets{};
    reshade::api::resource_view depth_stencil{};
    std::uint32_t render_target_count = 0;
    GBufferBinding gbuffer_binding = GBufferBinding::unavailable;
};

struct RangeKey {
    std::uintptr_t index_buffer = 0;
    std::uint32_t index_buffer_offset = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;

    bool operator==(const RangeKey&) const = default;
};

struct RangeKeyHash {
    std::size_t operator()(const RangeKey& key) const noexcept {
        std::size_t value = std::hash<std::uintptr_t>{}(key.index_buffer);
        value ^= std::hash<std::uint32_t>{}(key.index_buffer_offset) +
                 0x9E3779B9u + (value << 6u) + (value >> 2u);
        value ^= std::hash<std::uint32_t>{}(key.first_index) +
                 0x9E3779B9u + (value << 6u) + (value >> 2u);
        value ^= std::hash<std::uint32_t>{}(key.index_count) +
                 0x9E3779B9u + (value << 6u) + (value >> 2u);
        return value;
    }
};

struct SkinRange {
    std::uint32_t material_uid = 0;
    MaterialProfile profile = MaterialProfile::none;
    std::uint64_t last_seen_frame = 0;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
};

struct CapturedMaterial {
    std::uint32_t material_uid = 0;
    MaterialProfile profile = MaterialProfile::none;

    explicit operator bool() const noexcept {
        return material_uid != 0 && profile != MaterialProfile::none;
    }
};

using RenderSubmitFn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t, int);
using AlternateSubmitFn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                   std::uintptr_t, std::uintptr_t);

HMODULE g_module = nullptr;
std::atomic<std::uint32_t> g_ready_device_count = 0;
std::atomic<bool> g_native_hooks_ready = false;
// Native submit hooks point back into this add-on. The bundled MinHook fork
// frees a hook's trampoline when MH_RemoveHook is called, so the image and the
// trampolines are retained for process lifetime once installation starts.
// Acceptance is a separate behavioral gate: closing it prevents new material
// captures while the accepted-call counter drains before shared state is reset.
std::atomic<bool> g_native_hook_accepting = false;
std::atomic<std::uint32_t> g_native_hook_accepted_calls = 0;
std::atomic<bool> g_native_module_pinned = false;
std::atomic<bool> g_render_hook_installed = false;
std::atomic<bool> g_alternate_hook_installed = false;
std::atomic<std::uint64_t> g_frame = 0;
std::once_flag g_native_hooks_once;
std::mutex g_device_lifecycle_mutex;
std::shared_mutex g_skin_ranges_mutex;
using SkinRangesForBuffer = std::unordered_map<RangeKey, SkinRange, RangeKeyHash>;
std::unordered_map<std::uintptr_t, SkinRangesForBuffer> g_skin_ranges;
RenderSubmitFn g_original_render_submit = nullptr;
AlternateSubmitFn g_original_alternate_submit = nullptr;
void* g_render_submit_target = nullptr;
void* g_alternate_submit_target = nullptr;
thread_local bool g_inside_sss = false;

bool DrainAcceptedNativeHookCalls() noexcept;

class NativeHookCaptureGuard {
public:
    NativeHookCaptureGuard() noexcept {
        if (!g_native_hook_accepting.load(std::memory_order_acquire)) {
            return;
        }
        g_native_hook_accepted_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!g_native_hook_accepting.load(std::memory_order_acquire)) {
            g_native_hook_accepted_calls.fetch_sub(1, std::memory_order_release);
            return;
        }
        accepted_ = true;
    }

    NativeHookCaptureGuard(const NativeHookCaptureGuard&) = delete;
    NativeHookCaptureGuard& operator=(const NativeHookCaptureGuard&) = delete;

    ~NativeHookCaptureGuard() {
        if (accepted_) {
            g_native_hook_accepted_calls.fetch_sub(1, std::memory_order_release);
        }
    }

    bool accepted() const noexcept { return accepted_; }

private:
    bool accepted_ = false;
};

#if defined(SPATCH_SSS_DEVELOPMENT)
std::atomic<int> g_debug_view = 0;
std::atomic<bool> g_development_enabled = true;
bool g_f6_was_down = false;
bool g_f7_was_down = false;
constexpr std::array<const char*, 5> kDebugViewNames{
    "effect", "replay mask", "depth eligibility", "shader stencil",
    "material data"};
#endif

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
        "[ShenLong-SSS] ReShade callback dropped after %s%s%s; native rendering remains active.",
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

Settings LoadSettings() {
    Settings settings;
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        settings.enabled = false;
        return settings;
    }
    const std::wstring path = directory + L"ShenLong.ini";
    namespace ini = spatch::graphics::ini;
    constexpr auto enabled_keys = ini::SettingKeys(
        L"SubsurfaceScattering",
        L"SubsurfaceScattering",
        L"subsurface_scattering");
    constexpr auto quality_keys = ini::SettingKeys(
        L"SubsurfaceScattering", L"SSSQuality", L"sss_quality");
    constexpr auto strength_keys = ini::SettingKeys(
        L"SubsurfaceScattering", L"SSSStrength", L"sss_strength_percent");
    constexpr auto radius_keys = ini::SettingKeys(
        L"SubsurfaceScattering", L"SSSRadius", L"sss_radius_percent");
    constexpr auto eye_keys = ini::SettingKeys(
        L"MaterialScattering", L"EyeScattering", L"eye_scattering");
    constexpr auto hair_keys = ini::SettingKeys(
        L"MaterialScattering", L"HairScattering", L"hair_scattering");
    constexpr auto teeth_keys = ini::SettingKeys(
        L"MaterialScattering", L"TeethScattering", L"teeth_scattering");
    constexpr auto foliage_keys = ini::SettingKeys(
        L"MaterialScattering", L"FoliageTransmission",
        L"foliage_transmission");
    const bool spatch_enabled = ini::ReadBool(
        path, ini::kMasterEnabledKeys, false);
    const bool sss_enabled = ini::ReadBool(
        path, enabled_keys, settings.enabled);
    settings.skin_enabled = spatch_enabled && sss_enabled;
    settings.eye_enabled = spatch_enabled && ini::ReadBool(
        path, eye_keys, settings.eye_enabled);
    settings.hair_enabled = spatch_enabled && ini::ReadBool(
        path, hair_keys, settings.hair_enabled);
    settings.teeth_enabled = spatch_enabled && ini::ReadBool(
        path, teeth_keys, settings.teeth_enabled);
    settings.foliage_enabled = spatch_enabled && ini::ReadBool(
        path, foliage_keys, settings.foliage_enabled);
    settings.quality = (std::clamp)(
        ini::ReadInt(path, quality_keys, settings.quality),
        0, 2);
    settings.strength = static_cast<float>((std::clamp)(
        ini::ReadInt(path, strength_keys, 75),
        0, 100)) / 100.0f;
    settings.radius_scale = static_cast<float>((std::clamp)(
        ini::ReadInt(path, radius_keys, 100),
        25, 400)) / 100.0f;
    if (settings.strength <= 0.0f) {
        settings.skin_enabled = false;
        settings.eye_enabled = false;
        settings.hair_enabled = false;
        settings.teeth_enabled = false;
        settings.foliage_enabled = false;
    }
    settings.enabled = settings.skin_enabled || settings.eye_enabled ||
        settings.hair_enabled || settings.teeth_enabled ||
        settings.foliage_enabled;
    Log(reshade::log::level::info,
        "[ShenLong-SSS] config: enabled=%d skin=%d eye=%d hair=%d teeth=%d foliage=%d quality=%d strength=%.0f%% radius=%.0f%%",
        settings.enabled ? 1 : 0,
        settings.skin_enabled ? 1 : 0,
        settings.eye_enabled ? 1 : 0,
        settings.hair_enabled ? 1 : 0,
        settings.teeth_enabled ? 1 : 0,
        settings.foliage_enabled ? 1 : 0,
        settings.quality,
        settings.strength * 100.0f,
        settings.radius_scale * 100.0f);
    return settings;
}

bool IsProfileEnabled(
    const Settings& settings, MaterialProfile profile) noexcept {
    switch (profile) {
    case MaterialProfile::skin:
        return settings.skin_enabled;
    case MaterialProfile::eye:
        return settings.eye_enabled;
    case MaterialProfile::hair:
        return settings.hair_enabled;
    case MaterialProfile::teeth:
        return settings.teeth_enabled;
    case MaterialProfile::foliage:
        return settings.foliage_enabled;
    default:
        return false;
    }
}

void RefreshSettingsEnabled(Settings& settings) noexcept {
    settings.enabled = settings.skin_enabled || settings.eye_enabled ||
        settings.hair_enabled || settings.teeth_enabled ||
        settings.foliage_enabled;
}

bool RequiresMaterialData(const Settings& settings) noexcept {
    return settings.eye_enabled || settings.hair_enabled ||
        settings.foliage_enabled;
}

bool IsExactProfileShaderBinding(
    MaterialProfile profile,
    std::uint32_t pixel_shader_hash,
    std::uint32_t vertex_shader_hash) noexcept {
    switch (profile) {
    case MaterialProfile::skin:
    case MaterialProfile::teeth:
        return vertex_shader_hash == kSkinnedGBufferVertexShaderHash;
    case MaterialProfile::eye:
        return pixel_shader_hash == kEyeGBufferPixelShaderHash &&
            vertex_shader_hash == kEyeGBufferVertexShaderHash;
    case MaterialProfile::hair:
        return pixel_shader_hash == kHairGBufferPixelShaderHash &&
            vertex_shader_hash == kHairGBufferVertexShaderHash;
    case MaterialProfile::foliage:
        return pixel_shader_hash == kFoliageGBufferPixelShaderHash &&
            (vertex_shader_hash == kFoliageGBufferVertexShaderHash ||
             vertex_shader_hash == kFoliageGBufferWnVertexShaderHash);
    default:
        return false;
    }
}

bool BuildEyeCaptureConstants(
    std::uint32_t material_uid,
    CaptureConstants& constants) noexcept {
    const EyeMaterialProfile* profile = FindEyeMaterialProfile(material_uid);
    if (profile == nullptr) {
        return false;
    }
    constants = {};
    constants.eye_center[0] = profile->center_u;
    constants.eye_center[1] = profile->center_v;
    constants.eye_radius[0] = profile->radius_u;
    constants.eye_radius[1] = profile->radius_v;
    constants.eye_iris_inner = kEyeIrisInnerRadius;
    constants.eye_iris_outer = kEyeIrisOuterRadius;
    return true;
}

bool ReadPointer(std::uintptr_t address, std::uintptr_t& value) noexcept {
    value = 0;
    __try {
        value = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename Value>
bool ReadValue(std::uintptr_t address, Value& value) noexcept {
    value = {};
    __try {
        value = *reinterpret_cast<const Value*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ResolvedIndexBuffer {
    ID3D11Buffer* buffer = nullptr;
    std::uint32_t byte_offset = 0;
};

ResolvedIndexBuffer ResolveIndexBuffer(std::uintptr_t material_block) noexcept {
    std::uintptr_t geometry_stream = 0;
    std::uintptr_t platform_stream = 0;
    std::uintptr_t index_buffer = 0;
    std::uint32_t byte_offset = 0;
    if (!ReadPointer(material_block + 0x60, geometry_stream) ||
        geometry_stream == 0 ||
        !ReadPointer(geometry_stream + 0x88, platform_stream) ||
        platform_stream == 0 ||
        !ReadPointer(platform_stream + 0x38, index_buffer) ||
        !ReadValue(platform_stream + 0x40, byte_offset)) {
        return {};
    }
    return {reinterpret_cast<ID3D11Buffer*>(index_buffer), byte_offset};
}

void CaptureSkinRanges(std::uintptr_t node) noexcept {
    if (g_ready_device_count.load(std::memory_order_acquire) == 0 ||
        !g_native_hooks_ready.load(std::memory_order_acquire) || node == 0) {
        return;
    }

    std::uintptr_t material_root = 0;
    std::uint32_t material_count = 0;
    if (!ReadPointer(node + 0x28, material_root) || material_root == 0 ||
        !ReadValue(material_root + 0xA0, material_count) ||
        material_count == 0 || material_count > 64) {
        return;
    }

    struct PendingRange {
        RangeKey key;
        std::uint32_t material_uid = 0;
        MaterialProfile profile = MaterialProfile::none;
    };
    std::array<PendingRange, 64> pending{};
    std::size_t pending_count = 0;

    const std::uintptr_t first_block_offset =
        (0x120u + static_cast<std::uintptr_t>(material_count) * sizeof(void*) + 0x0Fu) &
        ~static_cast<std::uintptr_t>(0x0Fu);
    ResolvedIndexBuffer current_index_buffer{};
    for (std::uint32_t index = 0; index < material_count; ++index) {
        const std::uintptr_t block = material_root + first_block_offset +
            static_cast<std::uintptr_t>(index) * 0x110u;
        const ResolvedIndexBuffer resolved = ResolveIndexBuffer(block);
        if (resolved.buffer != nullptr) {
            current_index_buffer = resolved;
        }
        std::uint32_t material_uid = 0;
        std::uint32_t topology = 0;
        std::uint32_t first_index = 0;
        std::uint32_t primitive_count = 0;
        if (!ReadValue(block + 0x38, material_uid) ||
            !ReadValue(block + 0x100, topology) ||
            !ReadValue(block + 0x104, first_index) ||
            !ReadValue(block + 0x108, primitive_count) || topology != 3 ||
            primitive_count == 0 ||
            primitive_count > (std::numeric_limits<std::uint32_t>::max)() / 3u) {
            continue;
        }

        const MaterialProfile profile = ClassifyMaterial(material_uid);
        if (profile == MaterialProfile::none ||
            current_index_buffer.buffer == nullptr) {
            continue;
        }
        pending[pending_count++] = PendingRange{
            RangeKey{reinterpret_cast<std::uintptr_t>(current_index_buffer.buffer),
                      current_index_buffer.byte_offset, first_index,
                      primitive_count * 3u},
            material_uid,
            profile};
    }

    if (pending_count == 0) {
        return;
    }
    try {
        const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
        std::unique_lock lock(g_skin_ranges_mutex);
        for (std::size_t index = 0; index < pending_count; ++index) {
            const PendingRange& range = pending[index];
            auto& buffer_ranges = g_skin_ranges[range.key.index_buffer];
            auto [entry, inserted] = buffer_ranges.try_emplace(range.key);
            (void)inserted;
            entry->second.material_uid = range.material_uid;
            entry->second.profile = range.profile;
            entry->second.last_seen_frame = frame;
        }
    } catch (...) {
    }
}

CapturedMaterial FindCapturedMaterial(
    ID3D11Buffer* index_buffer,
    DXGI_FORMAT index_format,
    std::uint32_t index_buffer_offset,
    std::uint32_t first_index,
    std::uint32_t index_count) noexcept {
    if (index_buffer == nullptr || index_count == 0 ||
        (index_format != DXGI_FORMAT_R16_UINT &&
         index_format != DXGI_FORMAT_R32_UINT)) {
        return {};
    }
    try {
        const RangeKey key{reinterpret_cast<std::uintptr_t>(index_buffer),
                           index_buffer_offset, first_index, index_count};
        {
            std::shared_lock lock(g_skin_ranges_mutex);
            const auto buffer = g_skin_ranges.find(key.index_buffer);
            if (buffer == g_skin_ranges.end()) {
                return {};
            }
            const auto entry = buffer->second.find(key);
            if (entry == buffer->second.end()) {
                return {};
            }
            if (entry->second.index_format != DXGI_FORMAT_UNKNOWN) {
                return entry->second.index_format == index_format
                    ? CapturedMaterial{
                          entry->second.material_uid, entry->second.profile}
                    : CapturedMaterial{};
            }
        }
        std::unique_lock lock(g_skin_ranges_mutex);
        const auto buffer = g_skin_ranges.find(key.index_buffer);
        if (buffer == g_skin_ranges.end()) {
            return {};
        }
        const auto entry = buffer->second.find(key);
        if (entry == buffer->second.end()) {
            return {};
        }
        if (entry->second.index_format == DXGI_FORMAT_UNKNOWN) {
            entry->second.index_format = index_format;
        }
        return entry->second.index_format == index_format
            ? CapturedMaterial{entry->second.material_uid, entry->second.profile}
            : CapturedMaterial{};
    } catch (...) {
        return {};
    }
}

void PruneSkinRanges(std::uint64_t frame) noexcept {
    if (frame % 120 != 0) {
        return;
    }
    try {
        std::unique_lock lock(g_skin_ranges_mutex);
        for (auto buffer = g_skin_ranges.begin(); buffer != g_skin_ranges.end();) {
            std::erase_if(buffer->second, [frame](const auto& entry) {
                return frame > entry.second.last_seen_frame &&
                    frame - entry.second.last_seen_frame > kRangeLifetimeFrames;
            });
            if (buffer->second.empty()) {
                buffer = g_skin_ranges.erase(buffer);
            } else {
                ++buffer;
            }
        }
    } catch (...) {
    }
}

void DetourRenderSubmit(
    std::uintptr_t renderable,
    std::uintptr_t command,
    std::uintptr_t node,
    std::uintptr_t transform,
    std::uintptr_t geometry,
    int override_index) {
    NativeHookCaptureGuard capture_guard;
    if (capture_guard.accepted()) {
        CaptureSkinRanges(node);
    }
    const RenderSubmitFn original = g_original_render_submit;
    if (original != nullptr) {
        original(renderable, command, node, transform, geometry, override_index);
    }
}

void DetourAlternateSubmit(
    std::uintptr_t renderable,
    std::uintptr_t command,
    std::uintptr_t node,
    std::uintptr_t transform,
    std::uintptr_t geometry) {
    NativeHookCaptureGuard capture_guard;
    if (capture_guard.accepted()) {
        CaptureSkinRanges(node);
    }
    const AlternateSubmitFn original = g_original_alternate_submit;
    if (original != nullptr) {
        original(renderable, command, node, transform, geometry);
    }
}

template <std::size_t Size>
void* FindUniqueExecutableSignature(
    HMODULE module,
    const std::array<std::uint8_t, Size>& signature) noexcept {
    if (module == nullptr) {
        return nullptr;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }

    void* match = nullptr;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (std::uint16_t section_index = 0;
         section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section->Misc.VirtualSize < Size) {
            continue;
        }
        const auto* begin = reinterpret_cast<const std::uint8_t*>(
            base + section->VirtualAddress);
        const std::size_t length = section->Misc.VirtualSize;
        for (std::size_t offset = 0; offset + Size <= length; ++offset) {
            if (std::memcmp(begin + offset, signature.data(), Size) != 0) {
                continue;
            }
            if (match != nullptr) {
                return nullptr;
            }
            match = const_cast<std::uint8_t*>(begin + offset);
        }
    }
    return match;
}

bool PinNativeHookModule() noexcept {
    if (g_native_module_pinned.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_module == nullptr) {
        return false;
    }

    HMODULE pinned_module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(g_module),
            &pinned_module)) {
        Log(reshade::log::level::error,
            "[ShenLong-SSS] native hooks rejected because the add-on image "
            "could not be pinned for process lifetime (error=%lu).",
            GetLastError());
        return false;
    }
    g_native_module_pinned.store(true, std::memory_order_release);
    return true;
}

bool InstallNativeHooks() noexcept {
    g_native_hook_accepting.store(false, std::memory_order_release);
    g_native_hooks_ready.store(false, std::memory_order_release);
    const spatch::graphics::native::ExecutableProfile* const profile =
        spatch::graphics::native::GetVerifiedExecutableProfile();
    if (profile == nullptr) {
        Log(reshade::log::level::warning,
            "[ShenLong-SSS] native submit hooks rejected because the "
            "executable identity is not an exact supported ShenLong profile.");
        return false;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    g_render_submit_target = FindUniqueExecutableSignature(
        executable, kRenderSubmitSignature);
    g_alternate_submit_target = FindUniqueExecutableSignature(
        executable, kAlternateSubmitSignature);
    if (g_render_submit_target == nullptr || g_alternate_submit_target == nullptr) {
        return false;
    }
    if (!PinNativeHookModule()) {
        g_render_submit_target = nullptr;
        g_alternate_submit_target = nullptr;
        return false;
    }

    // ShenLong uses SDmodding's x64 MinHook fork. MH_CreateHook installs the
    // detour immediately; there is no separate initialize/enable phase.
    const MH_STATUS render_status = MH_CreateHook(
        g_render_submit_target,
        reinterpret_cast<void*>(&DetourRenderSubmit),
        reinterpret_cast<void**>(&g_original_render_submit));
    if (render_status != MH_OK) {
        g_original_render_submit = nullptr;
        g_render_submit_target = nullptr;
        g_alternate_submit_target = nullptr;
        return false;
    }
    g_render_hook_installed.store(true, std::memory_order_release);

    const MH_STATUS alternate_status = MH_CreateHook(
        g_alternate_submit_target,
        reinterpret_cast<void*>(&DetourAlternateSubmit),
        reinterpret_cast<void**>(&g_original_alternate_submit));
    if (alternate_status != MH_OK) {
        // MH_RemoveHook frees the first hook's original trampoline and can
        // race a detour which already loaded it. Keep the transparent partial
        // hook and pinned image alive for the rest of the process instead.
        g_original_alternate_submit = nullptr;
        g_alternate_submit_target = nullptr;
        Log(reshade::log::level::warning,
            "[ShenLong-SSS] alternate submit hook failed (status=%d); the "
            "transparent render-submit hook is retained for process lifetime.",
            static_cast<int>(alternate_status));
        return false;
    }
    g_alternate_hook_installed.store(true, std::memory_order_release);
    g_native_hooks_ready.store(true, std::memory_order_release);
    g_native_hook_accepting.store(true, std::memory_order_release);
    return true;
}

#if defined(SPATCH_SSS_DEVELOPMENT)
bool CompileShader(
    const std::wstring& path,
    const char* entry_point,
    const char* target,
    const D3D_SHADER_MACRO* macros,
    ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const HRESULT result = D3DCompileFromFile(
        path.c_str(), macros, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry_point, target, flags, 0,
        bytecode.ReleaseAndGetAddressOf(), errors.GetAddressOf());
    if (FAILED(result) || !bytecode) {
        const char* detail = errors && errors->GetBufferPointer()
            ? static_cast<const char*>(errors->GetBufferPointer())
            : "no compiler diagnostics";
        Log(reshade::log::level::error,
            "[ShenLong-SSS] shader compile failed for %s: %s",
            entry_point, detail);
        return false;
    }
    return true;
}
#endif

bool LoadShaderBytecode(
    const std::wstring& cache_path,
    const std::wstring& shader_path,
    const char* entry_point,
    const char* target,
    const D3D_SHADER_MACRO* macros,
    ComPtr<ID3DBlob>& bytecode,
    bool& used_source_fallback) {
    const HRESULT load_result = D3DReadFileToBlob(
        cache_path.c_str(), bytecode.ReleaseAndGetAddressOf());
    if (SUCCEEDED(load_result) && bytecode) {
        return true;
    }

#if defined(SPATCH_SSS_DEVELOPMENT)
    used_source_fallback = true;
    Log(reshade::log::level::warning,
        "[ShenLong-SSS] Precompiled shader cache unavailable for %s/%s "
        "(HRESULT=0x%08X); using the Development source fallback.",
        entry_point,
        target,
        static_cast<unsigned int>(load_result));
    return CompileShader(
        shader_path, entry_point, target, macros, bytecode);
#else
    static_cast<void>(shader_path);
    static_cast<void>(macros);
    static_cast<void>(used_source_fallback);
    Log(reshade::log::level::error,
        "[ShenLong-SSS] Required precompiled shader cache entry is missing or "
        "invalid for %s/%s (HRESULT=0x%08X).",
        entry_point,
        target,
        static_cast<unsigned int>(load_result));
    return false;
#endif
}

#if defined(SPATCH_SSS_DEVELOPMENT)
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

bool InitializeShadersAndStates(ID3D11Device* device, DeviceData& data) {
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        return false;
    }
    const std::wstring shader_path =
        directory + L"ShenLong\\Shaders\\SSS\\SPatchSSS.hlsl";
    const std::wstring cache_root =
        directory + L"ShenLong\\ShaderCache\\v1\\SSS\\";
    static constexpr std::array<const char*, 3> quality_values{"0", "1", "2"};
#if defined(SPATCH_SSS_DEVELOPMENT)
    constexpr const char* development_value = "1";
    constexpr const wchar_t* development_cache_value = L"1";
#else
    constexpr const char* development_value = "0";
    constexpr const wchar_t* development_cache_value = L"0";
#endif

    bool used_source_fallback = false;
    ComPtr<ID3DBlob> vertex_bytecode;
    if (!LoadShaderBytecode(
            cache_root + L"FullscreenVS.vs_5_0.cso",
            shader_path,
            "FullscreenVS",
            "vs_5_0",
            nullptr,
            vertex_bytecode,
            used_source_fallback) ||
        FAILED(device->CreateVertexShader(
            vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize(),
            nullptr, data.fullscreen_vertex_shader.ReleaseAndGetAddressOf()))) {
        return false;
    }

    for (int horizontal = 0; horizontal <= 1; ++horizontal) {
        const D3D_SHADER_MACRO macros[] = {
            {"SPATCH_SSS_QUALITY", quality_values[data.settings.quality]},
            {"SPATCH_SSS_HORIZONTAL", horizontal != 0 ? "1" : "0"},
            {"SPATCH_SSS_DEVELOPMENT", development_value},
            {nullptr, nullptr}};
        ComPtr<ID3DBlob> pixel_bytecode;
        const std::wstring cache_path =
            cache_root + L"BlurPS.ps_5_0.q" +
            std::to_wstring(data.settings.quality) + L".horizontal" +
            (horizontal != 0 ? L"1" : L"0") + L".development" +
            development_cache_value + L".cso";
        if (!LoadShaderBytecode(
                cache_path, shader_path, "BlurPS", "ps_5_0", macros,
                pixel_bytecode, used_source_fallback)) {
            return false;
        }
        ComPtr<ID3D11PixelShader>& target = horizontal != 0
            ? data.horizontal_pixel_shader
            : data.vertical_pixel_shader;
        if (FAILED(device->CreatePixelShader(
                pixel_bytecode->GetBufferPointer(), pixel_bytecode->GetBufferSize(),
                nullptr, target.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    const auto initialize_optional_pixel_shader = [
        &device, &cache_root, &shader_path, &used_source_fallback](
            bool& profile_enabled,
            const wchar_t* cache_name,
            const char* entry_point,
            const char* target,
            const char* profile_name,
            ComPtr<ID3D11PixelShader>& shader) {
        if (!profile_enabled) {
            return;
        }
        ComPtr<ID3DBlob> pixel_bytecode;
        const bool loaded = LoadShaderBytecode(
            cache_root + cache_name, shader_path, entry_point, target, nullptr,
            pixel_bytecode, used_source_fallback);
        const HRESULT create_result = loaded
            ? device->CreatePixelShader(
                  pixel_bytecode->GetBufferPointer(),
                  pixel_bytecode->GetBufferSize(), nullptr,
                  shader.ReleaseAndGetAddressOf())
            : E_FAIL;
        if (!loaded || FAILED(create_result)) {
            profile_enabled = false;
            shader.Reset();
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] %s shader initialization failed; that optional profile is disabled while other profiles remain available.",
                profile_name);
        }
    };
    initialize_optional_pixel_shader(
        data.settings.eye_enabled, L"EyeMaskPS.ps_4_0.cso", "EyeMaskPS",
        "ps_4_0", "eye", data.eye_mask_pixel_shader);
    initialize_optional_pixel_shader(
        data.settings.hair_enabled, L"HairCapturePS.ps_4_0.cso",
        "HairCapturePS", "ps_4_0", "hair", data.hair_capture_pixel_shader);
    initialize_optional_pixel_shader(
        data.settings.foliage_enabled, L"FoliageCapturePS.ps_4_0.cso",
        "FoliageCapturePS", "ps_4_0", "foliage",
        data.foliage_capture_pixel_shader);
    initialize_optional_pixel_shader(
        data.settings.foliage_enabled,
        L"FoliageTransmissionPS.ps_5_0.cso", "FoliageTransmissionPS",
        "ps_5_0", "foliage", data.foliage_transmission_pixel_shader);
    if (!data.settings.foliage_enabled) {
        data.foliage_capture_pixel_shader.Reset();
        data.foliage_transmission_pixel_shader.Reset();
    }
    RefreshSettingsEnabled(data.settings);
    if (used_source_fallback) {
        Log(reshade::log::level::info,
            "[ShenLong-SSS] Shader bytecode initialized with the Development "
            "source fallback.");
    } else {
        Log(reshade::log::level::info,
            "[ShenLong-SSS] Precompiled shader cache v1 loaded "
            "(quality=%d, development=%s).",
            data.settings.quality,
            development_value);
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(
            &sampler_desc, data.linear_sampler.ReleaseAndGetAddressOf()))) {
        return false;
    }
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(
            &blend_desc, data.no_blend_state.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC mask_desc{};
    mask_desc.DepthEnable = TRUE;
    mask_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    mask_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    mask_desc.StencilEnable = TRUE;
    mask_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    mask_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    mask_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    mask_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    mask_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    mask_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    mask_desc.BackFace = mask_desc.FrontFace;
    if (FAILED(device->CreateDepthStencilState(
            &mask_desc, data.mask_write_state.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC blur_desc{};
    blur_desc.DepthEnable = FALSE;
    blur_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    blur_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    blur_desc.StencilEnable = TRUE;
    blur_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    blur_desc.StencilWriteMask = 0;
    blur_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    blur_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    blur_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    blur_desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    blur_desc.BackFace = blur_desc.FrontFace;
    if (FAILED(device->CreateDepthStencilState(
            &blur_desc, data.blur_stencil_state.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(
            &raster_desc, data.fullscreen_rasterizer_state.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_BUFFER_DESC constant_desc{};
    constant_desc.ByteWidth = sizeof(SssConstants);
    constant_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(
            &constant_desc, nullptr, data.constants.ReleaseAndGetAddressOf()))) {
        return false;
    }
    if (data.settings.eye_enabled) {
        constant_desc.ByteWidth = sizeof(CaptureConstants);
        if (FAILED(device->CreateBuffer(
                &constant_desc, nullptr,
                data.capture_constants.ReleaseAndGetAddressOf()))) {
            data.settings.eye_enabled = false;
            data.eye_mask_pixel_shader.Reset();
            RefreshSettingsEnabled(data.settings);
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] eye capture constants allocation failed; the optional eye profile is disabled while other profiles remain available.");
        }
    }
#if defined(SPATCH_SSS_DEVELOPMENT)
    ResetGpuTiming(device, data);
#endif
    return true;
}

void ResetLightingResources(DeviceData& data) noexcept {
    data.original_lighting.Reset();
    data.original_lighting_srv.Reset();
    data.temporary_lighting.Reset();
    data.temporary_lighting_rtv.Reset();
    data.temporary_lighting_srv.Reset();
    data.output_lighting_resource.Reset();
    data.output_lighting_rtv.Reset();
    data.lighting_format = DXGI_FORMAT_UNKNOWN;
}

void ResetFrameResources(DeviceData& data) noexcept {
    data.mask_depth.Reset();
    data.mask_write_dsv.Reset();
    data.mask_read_dsv.Reset();
    data.mask_depth_srv.Reset();
    data.mask_stencil_srv.Reset();
    data.material_data.Reset();
    data.material_data_rtv.Reset();
    data.material_data_srv.Reset();
    data.mask_source_desc = {};
    data.mask_source_desc_valid = false;
    data.scene_depth_resource.Reset();
    data.scene_depth_srv.Reset();
    ResetLightingResources(data);
    data.width = 0;
    data.height = 0;
    data.mask_frame = (std::numeric_limits<std::uint64_t>::max)();
    data.processed_frame = (std::numeric_limits<std::uint64_t>::max)();
    data.mask_draw_counts = {};
}

bool EqualTextureDescription(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right) noexcept {
    return left.Width == right.Width && left.Height == right.Height &&
        left.MipLevels == right.MipLevels && left.ArraySize == right.ArraySize &&
        left.Format == right.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality &&
        left.Usage == right.Usage && left.BindFlags == right.BindFlags &&
        left.CPUAccessFlags == right.CPUAccessFlags &&
        left.MiscFlags == right.MiscFlags;
}

bool CreateMaskResources(
    DeviceData& data,
    const D3D11_TEXTURE2D_DESC& source_desc) {
    if (source_desc.Width == 0 || source_desc.Height == 0 ||
        source_desc.Format != DXGI_FORMAT_R24G8_TYPELESS ||
        source_desc.SampleDesc.Count != 1 || source_desc.MipLevels != 1 ||
        source_desc.ArraySize != 1) {
        ResetFrameResources(data);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    const bool needs_material_data = RequiresMaterialData(data.settings);
    const bool cached_material_data_ready = !needs_material_data ||
        (data.material_data && data.material_data_rtv && data.material_data_srv);
    if (data.mask_depth && data.mask_write_dsv && data.mask_read_dsv &&
        data.mask_depth_srv && data.mask_stencil_srv &&
        cached_material_data_ready &&
        data.mask_source_desc_valid &&
        EqualTextureDescription(data.mask_source_desc, source_desc)) {
        D3D11_TEXTURE2D_DESC cached_desc{};
        data.mask_depth->GetDesc(&cached_desc);
        if (EqualTextureDescription(cached_desc, desc)) {
            data.width = source_desc.Width;
            data.height = source_desc.Height;
            return true;
        }
    }

    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    const bool repeated_failure = data.failed_mask_source_desc_valid &&
        EqualTextureDescription(data.failed_mask_source_desc, source_desc);
    if (repeated_failure && frame < data.mask_retry_after_frame) {
        return false;
    }
    if (!repeated_failure) {
        data.logged_mask_allocation_failure = false;
    }
    const auto record_failure = [&data, &source_desc, frame]() noexcept {
        data.failed_mask_source_desc = source_desc;
        data.failed_mask_source_desc_valid = true;
        data.mask_retry_after_frame = frame + 120;
        if (!data.logged_mask_allocation_failure) {
            data.logged_mask_allocation_failure = true;
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] mask allocation failed at %ux%u; the native frame is unchanged and retry is rate-limited.",
                source_desc.Width,
                source_desc.Height);
        }
        return false;
    };

    ComPtr<ID3D11Texture2D> mask_depth;
    ComPtr<ID3D11DepthStencilView> mask_write_dsv;
    ComPtr<ID3D11DepthStencilView> mask_read_dsv;
    ComPtr<ID3D11ShaderResourceView> mask_depth_srv;
    ComPtr<ID3D11ShaderResourceView> mask_stencil_srv;
    ComPtr<ID3D11Texture2D> material_data;
    ComPtr<ID3D11RenderTargetView> material_data_rtv;
    ComPtr<ID3D11ShaderResourceView> material_data_srv;
    if (FAILED(data.native_device->CreateTexture2D(
            &desc, nullptr, mask_depth.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(data.native_device->CreateDepthStencilView(
            mask_depth.Get(), &dsv_desc,
            mask_write_dsv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }
    dsv_desc.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    if (FAILED(data.native_device->CreateDepthStencilView(
            mask_depth.Get(), &dsv_desc,
            mask_read_dsv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if (FAILED(data.native_device->CreateShaderResourceView(
            mask_depth.Get(), &srv_desc,
            mask_depth_srv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }
    srv_desc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    if (FAILED(data.native_device->CreateShaderResourceView(
            mask_depth.Get(), &srv_desc,
            mask_stencil_srv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }
    if (needs_material_data) {
        D3D11_TEXTURE2D_DESC material_desc = desc;
        material_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        material_desc.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(data.native_device->CreateTexture2D(
                &material_desc, nullptr,
                material_data.ReleaseAndGetAddressOf())) ||
            FAILED(data.native_device->CreateRenderTargetView(
                material_data.Get(), nullptr,
                material_data_rtv.ReleaseAndGetAddressOf())) ||
            FAILED(data.native_device->CreateShaderResourceView(
                material_data.Get(), nullptr,
                material_data_srv.ReleaseAndGetAddressOf()))) {
            material_data.Reset();
            material_data_rtv.Reset();
            material_data_srv.Reset();
            data.settings.eye_enabled = false;
            data.settings.hair_enabled = false;
            data.settings.foliage_enabled = false;
            RefreshSettingsEnabled(data.settings);
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] optional material-data allocation failed at %ux%u; eye, hair, and foliage scattering are disabled while skin and teeth remain available.",
                source_desc.Width,
                source_desc.Height);
        }
    }
    ResetFrameResources(data);
    data.mask_depth = std::move(mask_depth);
    data.mask_write_dsv = std::move(mask_write_dsv);
    data.mask_read_dsv = std::move(mask_read_dsv);
    data.mask_depth_srv = std::move(mask_depth_srv);
    data.mask_stencil_srv = std::move(mask_stencil_srv);
    data.material_data = std::move(material_data);
    data.material_data_rtv = std::move(material_data_rtv);
    data.material_data_srv = std::move(material_data_srv);
    data.width = source_desc.Width;
    data.height = source_desc.Height;
    data.mask_source_desc = source_desc;
    data.mask_source_desc_valid = true;
    data.failed_mask_source_desc = {};
    data.failed_mask_source_desc_valid = false;
    data.mask_retry_after_frame = 0;
    data.logged_mask_allocation_failure = false;
    return true;
}

bool CreateSceneDepthView(
    DeviceData& data,
    ID3D11DepthStencilView* source_view) {
    if (source_view == nullptr) {
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    source_view->GetResource(resource.GetAddressOf());
    if (!resource || FAILED(resource.As(&texture))) {
        return false;
    }
    if (data.scene_depth_resource.Get() == texture.Get() && data.scene_depth_srv) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width != data.width || desc.Height != data.height ||
        desc.Format != DXGI_FORMAT_R24G8_TYPELESS || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1 ||
        (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        return false;
    }

    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    const std::uintptr_t source_identity =
        reinterpret_cast<std::uintptr_t>(texture.Get());
    if (data.failed_scene_depth_source == source_identity &&
        frame < data.scene_depth_retry_after_frame) {
        return false;
    }
    if (data.failed_scene_depth_source != source_identity) {
        data.logged_scene_depth_failure = false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(data.native_device->CreateShaderResourceView(
            texture.Get(), &view_desc, view.GetAddressOf()))) {
        data.failed_scene_depth_source = source_identity;
        data.scene_depth_retry_after_frame = frame + 120;
        if (!data.logged_scene_depth_failure) {
            data.logged_scene_depth_failure = true;
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] scene-depth view allocation failed; the native frame is unchanged and retry is rate-limited.");
        }
        return false;
    }
    data.scene_depth_resource = std::move(texture);
    data.scene_depth_srv = std::move(view);
    data.failed_scene_depth_source = 0;
    data.scene_depth_retry_after_frame = 0;
    data.logged_scene_depth_failure = false;
    return true;
}

bool CreateLightingResources(DeviceData& data, ID3D11Texture2D* source) {
    if (source == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    if (source_desc.Width != data.width || source_desc.Height != data.height ||
        source_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        source_desc.SampleDesc.Count != 1 || source_desc.MipLevels != 1 ||
        source_desc.ArraySize != 1) {
        return false;
    }
    if (data.original_lighting && data.original_lighting_srv &&
        data.temporary_lighting && data.temporary_lighting_rtv &&
        data.temporary_lighting_srv && data.output_lighting_resource &&
        data.output_lighting_rtv &&
        data.output_lighting_resource.Get() == source &&
        data.lighting_format == source_desc.Format) {
        return true;
    }

    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    const std::uintptr_t source_identity = reinterpret_cast<std::uintptr_t>(source);
    const bool repeated_failure = data.failed_lighting_desc_valid &&
        data.failed_lighting_source == source_identity &&
        EqualTextureDescription(data.failed_lighting_desc, source_desc);
    if (repeated_failure && frame < data.lighting_retry_after_frame) {
        return false;
    }
    if (!repeated_failure) {
        data.logged_lighting_allocation_failure = false;
    }
    const auto record_failure = [&data, &source_desc, source_identity, frame]() noexcept {
        data.failed_lighting_source = source_identity;
        data.failed_lighting_desc = source_desc;
        data.failed_lighting_desc_valid = true;
        data.lighting_retry_after_frame = frame + 120;
        if (!data.logged_lighting_allocation_failure) {
            data.logged_lighting_allocation_failure = true;
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] lighting allocation failed at %ux%u; the native frame is unchanged and retry is rate-limited.",
                source_desc.Width,
                source_desc.Height);
        }
        return false;
    };

    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> original_lighting;
    ComPtr<ID3D11ShaderResourceView> original_lighting_srv;
    ComPtr<ID3D11Texture2D> temporary_lighting;
    ComPtr<ID3D11RenderTargetView> temporary_lighting_rtv;
    ComPtr<ID3D11ShaderResourceView> temporary_lighting_srv;
    ComPtr<ID3D11Texture2D> output_lighting_resource;
    ComPtr<ID3D11RenderTargetView> output_lighting_rtv;
    if (FAILED(data.native_device->CreateTexture2D(
            &desc, nullptr, original_lighting.ReleaseAndGetAddressOf())) ||
        FAILED(data.native_device->CreateShaderResourceView(
            original_lighting.Get(), nullptr,
            original_lighting_srv.ReleaseAndGetAddressOf())) ||
        FAILED(data.native_device->CreateTexture2D(
            &desc, nullptr, temporary_lighting.ReleaseAndGetAddressOf())) ||
        FAILED(data.native_device->CreateRenderTargetView(
            temporary_lighting.Get(), nullptr,
            temporary_lighting_rtv.ReleaseAndGetAddressOf())) ||
        FAILED(data.native_device->CreateShaderResourceView(
            temporary_lighting.Get(), nullptr,
            temporary_lighting_srv.ReleaseAndGetAddressOf())) ||
        FAILED(source->QueryInterface(
            IID_PPV_ARGS(output_lighting_resource.ReleaseAndGetAddressOf()))) ||
        FAILED(data.native_device->CreateRenderTargetView(
            source, nullptr, output_lighting_rtv.ReleaseAndGetAddressOf()))) {
        return record_failure();
    }
    ResetLightingResources(data);
    data.original_lighting = std::move(original_lighting);
    data.original_lighting_srv = std::move(original_lighting_srv);
    data.temporary_lighting = std::move(temporary_lighting);
    data.temporary_lighting_rtv = std::move(temporary_lighting_rtv);
    data.temporary_lighting_srv = std::move(temporary_lighting_srv);
    data.output_lighting_resource = std::move(output_lighting_resource);
    data.output_lighting_rtv = std::move(output_lighting_rtv);
    data.lighting_format = source_desc.Format;
    data.failed_lighting_source = 0;
    data.failed_lighting_desc = {};
    data.failed_lighting_desc_valid = false;
    data.lighting_retry_after_frame = 0;
    data.logged_lighting_allocation_failure = false;
#if defined(SPATCH_SSS_DEVELOPMENT)
    ResetGpuTiming(data.native_device, data);
#endif
    return true;
}

#if defined(SPATCH_SSS_DEVELOPMENT)
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
        "[ShenLong-SSS] GPU timing at %ux%u: average=%.3f ms, p95=%.3f ms, "
        "maximum=%.3f ms (%zu frames).",
        data.width, data.height, average, sorted[p95_index], sorted.back(),
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
    GpuTimingScope(ID3D11DeviceContext* value, DeviceData& device_data) noexcept
        : context_(value), data_(device_data) {
        if (!data_.gpu_timing_ready || data_.gpu_timing_logged) {
            return;
        }
        for (std::size_t attempt = 0;
             attempt < data_.gpu_timing_slots.size();
             ++attempt) {
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
        if (slot_ == nullptr) {
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

struct SavedGraphicsState {
    explicit SavedGraphicsState(ID3D11DeviceContext* value) : context(value) {
        context->OMGetRenderTargets(
            static_cast<UINT>(render_targets.size()), render_targets.data(),
            &depth_stencil_view);
        context->OMGetBlendState(&blend_state, blend_factor.data(), &sample_mask);
        context->OMGetDepthStencilState(&depth_stencil_state, &stencil_reference);
        context->RSGetState(&rasterizer_state);
        viewport_count = static_cast<UINT>(viewports.size());
        context->RSGetViewports(&viewport_count, viewports.data());
        scissor_count = static_cast<UINT>(scissors.size());
        context->RSGetScissorRects(&scissor_count, scissors.data());
        context->IAGetInputLayout(&input_layout);
        context->IAGetPrimitiveTopology(&topology);
        vertex_class_count = static_cast<UINT>(vertex_class_instances.size());
        context->VSGetShader(
            &vertex_shader, vertex_class_instances.data(), &vertex_class_count);
        pixel_class_count = static_cast<UINT>(pixel_class_instances.size());
        context->PSGetShader(
            &pixel_shader, pixel_class_instances.data(), &pixel_class_count);
        geometry_class_count = static_cast<UINT>(geometry_class_instances.size());
        context->GSGetShader(
            &geometry_shader, geometry_class_instances.data(), &geometry_class_count);
        hull_class_count = static_cast<UINT>(hull_class_instances.size());
        context->HSGetShader(
            &hull_shader, hull_class_instances.data(), &hull_class_count);
        domain_class_count = static_cast<UINT>(domain_class_instances.size());
        context->DSGetShader(
            &domain_shader, domain_class_instances.data(), &domain_class_count);
        context->PSGetShaderResources(
            0, static_cast<UINT>(shader_resources.size()), shader_resources.data());
        context->PSGetSamplers(
            0, static_cast<UINT>(samplers.size()), samplers.data());
        context->PSGetConstantBuffers(
            0, static_cast<UINT>(constant_buffers.size()), constant_buffers.data());
        context->GetPredication(&predicate, &predicate_value);
        context->SetPredication(nullptr, FALSE);
    }

    SavedGraphicsState(const SavedGraphicsState&) = delete;
    SavedGraphicsState& operator=(const SavedGraphicsState&) = delete;

    ~SavedGraphicsState() {
        Restore();
        Release();
    }

    void Restore() noexcept {
        if (restored) {
            return;
        }
        context->OMSetRenderTargets(
            static_cast<UINT>(render_targets.size()), render_targets.data(),
            depth_stencil_view);
        context->OMSetBlendState(blend_state, blend_factor.data(), sample_mask);
        context->OMSetDepthStencilState(depth_stencil_state, stencil_reference);
        context->RSSetState(rasterizer_state);
        context->RSSetViewports(
            viewport_count, viewport_count != 0 ? viewports.data() : nullptr);
        context->RSSetScissorRects(
            scissor_count, scissor_count != 0 ? scissors.data() : nullptr);
        context->IASetInputLayout(input_layout);
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(
            vertex_shader, vertex_class_instances.data(), vertex_class_count);
        context->PSSetShader(
            pixel_shader, pixel_class_instances.data(), pixel_class_count);
        context->GSSetShader(
            geometry_shader, geometry_class_instances.data(), geometry_class_count);
        context->HSSetShader(
            hull_shader, hull_class_instances.data(), hull_class_count);
        context->DSSetShader(
            domain_shader, domain_class_instances.data(), domain_class_count);
        context->PSSetShaderResources(
            0, static_cast<UINT>(shader_resources.size()), shader_resources.data());
        context->PSSetSamplers(
            0, static_cast<UINT>(samplers.size()), samplers.data());
        context->PSSetConstantBuffers(
            0, static_cast<UINT>(constant_buffers.size()), constant_buffers.data());
        context->SetPredication(predicate, predicate_value);
        restored = true;
    }

    void Release() noexcept {
        for (ID3D11RenderTargetView* view : render_targets) {
            if (view != nullptr) view->Release();
        }
        if (depth_stencil_view != nullptr) depth_stencil_view->Release();
        if (blend_state != nullptr) blend_state->Release();
        if (depth_stencil_state != nullptr) depth_stencil_state->Release();
        if (rasterizer_state != nullptr) rasterizer_state->Release();
        if (input_layout != nullptr) input_layout->Release();
        if (vertex_shader != nullptr) vertex_shader->Release();
        if (pixel_shader != nullptr) pixel_shader->Release();
        if (geometry_shader != nullptr) geometry_shader->Release();
        if (hull_shader != nullptr) hull_shader->Release();
        if (domain_shader != nullptr) domain_shader->Release();
        const auto release_class_instances = [](
            auto& instances, UINT count) noexcept {
            for (UINT index = 0; index < count; ++index) {
                if (instances[index] != nullptr) instances[index]->Release();
            }
        };
        release_class_instances(vertex_class_instances, vertex_class_count);
        release_class_instances(pixel_class_instances, pixel_class_count);
        release_class_instances(geometry_class_instances, geometry_class_count);
        release_class_instances(hull_class_instances, hull_class_count);
        release_class_instances(domain_class_instances, domain_class_count);
        for (ID3D11ShaderResourceView* view : shader_resources) {
            if (view != nullptr) view->Release();
        }
        for (ID3D11SamplerState* sampler : samplers) {
            if (sampler != nullptr) sampler->Release();
        }
        for (ID3D11Buffer* buffer : constant_buffers) {
            if (buffer != nullptr) buffer->Release();
        }
        if (predicate != nullptr) predicate->Release();
    }

    ID3D11DeviceContext* context = nullptr;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        render_targets{};
    ID3D11DepthStencilView* depth_stencil_view = nullptr;
    ID3D11BlendState* blend_state = nullptr;
    std::array<float, 4> blend_factor{};
    UINT sample_mask = 0;
    ID3D11DepthStencilState* depth_stencil_state = nullptr;
    UINT stencil_reference = 0;
    ID3D11RasterizerState* rasterizer_state = nullptr;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports{};
    UINT viewport_count = 0;
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        scissors{};
    UINT scissor_count = 0;
    ID3D11InputLayout* input_layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11GeometryShader* geometry_shader = nullptr;
    ID3D11HullShader* hull_shader = nullptr;
    ID3D11DomainShader* domain_shader = nullptr;
    std::array<ID3D11ClassInstance*, 256> vertex_class_instances{};
    std::array<ID3D11ClassInstance*, 256> pixel_class_instances{};
    std::array<ID3D11ClassInstance*, 256> geometry_class_instances{};
    std::array<ID3D11ClassInstance*, 256> hull_class_instances{};
    std::array<ID3D11ClassInstance*, 256> domain_class_instances{};
    UINT vertex_class_count = 0;
    UINT pixel_class_count = 0;
    UINT geometry_class_count = 0;
    UINT hull_class_count = 0;
    UINT domain_class_count = 0;
    std::array<ID3D11ShaderResourceView*, 9> shader_resources{};
    std::array<ID3D11SamplerState*, 1> samplers{};
    std::array<ID3D11Buffer*, 2> constant_buffers{};
    ID3D11Predicate* predicate = nullptr;
    BOOL predicate_value = FALSE;
    bool restored = false;
};

bool InitializeMaskForFrame(
    ID3D11DeviceContext* context,
    DeviceData& data,
    ID3D11DepthStencilView* game_dsv,
    std::uint64_t frame) {
    ComPtr<ID3D11Resource> game_depth_resource;
    game_dsv->GetResource(game_depth_resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> game_depth;
    if (!game_depth_resource || FAILED(game_depth_resource.As(&game_depth))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC depth_desc{};
    game_depth->GetDesc(&depth_desc);
    if (!CreateMaskResources(data, depth_desc)) {
        return false;
    }
    if (data.mask_frame == frame) {
        return true;
    }

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Predicate* predicate = nullptr;
    BOOL predicate_value = FALSE;
    context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
    context->GetPredication(&predicate, &predicate_value);
    context->SetPredication(nullptr, FALSE);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(data.mask_depth.Get(), game_depth.Get());
    context->ClearDepthStencilView(
        data.mask_write_dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);
    if (data.material_data_rtv) {
        constexpr FLOAT kNoMaterialData[4]{};
        context->ClearRenderTargetView(
            data.material_data_rtv.Get(), kNoMaterialData);
    }
    context->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
    context->SetPredication(predicate, predicate_value);
    for (ID3D11RenderTargetView* view : rtvs) {
        if (view != nullptr) view->Release();
    }
    if (dsv != nullptr) dsv->Release();
    if (predicate != nullptr) predicate->Release();

    data.mask_frame = frame;
    data.mask_draw_counts = {};
    return true;
}

bool UpdateCaptureConstants(
    ID3D11DeviceContext* context,
    DeviceData& data,
    const CaptureConstants& constants) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            data.capture_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
            &mapped)) ||
        mapped.pData == nullptr) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(data.capture_constants.Get(), 0);
    return true;
}

bool WriteMaterialRangeToMask(
    ID3D11DeviceContext* context,
    DeviceData& data,
    CapturedMaterial material,
    const CaptureConstants* capture_constants,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) {
    if (!material || !IsProfileEnabled(data.settings, material.profile)) {
        return false;
    }
    const std::size_t profile_index = static_cast<std::size_t>(material.profile);
    if (profile_index >= data.mask_draw_counts.size()) {
        return false;
    }
    if (material.profile == MaterialProfile::eye &&
        (capture_constants == nullptr ||
         !UpdateCaptureConstants(context, data, *capture_constants))) {
        return false;
    }

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
    if (dsv == nullptr) {
        for (ID3D11RenderTargetView* view : rtvs) {
            if (view != nullptr) view->Release();
        }
        return false;
    }

    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (!InitializeMaskForFrame(context, data, dsv, frame)) {
        for (ID3D11RenderTargetView* view : rtvs) {
            if (view != nullptr) view->Release();
        }
        dsv->Release();
        return false;
    }
    if (!IsProfileEnabled(data.settings, material.profile)) {
        for (ID3D11RenderTargetView* view : rtvs) {
            if (view != nullptr) view->Release();
        }
        dsv->Release();
        return false;
    }

    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11BlendState* blend_state = nullptr;
    FLOAT blend_factor[4]{};
    UINT sample_mask = 0;
    ID3D11DepthStencilState* depth_state = nullptr;
    UINT stencil_reference = 0;
    ID3D11Buffer* previous_capture_constants = nullptr;
    std::array<ID3D11ClassInstance*, 256> pixel_class_instances{};
    UINT pixel_class_count = static_cast<UINT>(pixel_class_instances.size());
    context->PSGetShader(
        &pixel_shader, pixel_class_instances.data(), &pixel_class_count);
    context->OMGetBlendState(&blend_state, blend_factor, &sample_mask);
    context->OMGetDepthStencilState(&depth_state, &stencil_reference);
    context->PSGetConstantBuffers(5, 1, &previous_capture_constants);

    ID3D11RenderTargetView* capture_rtv = nullptr;
    ID3D11PixelShader* capture_shader = nullptr;
    if (material.profile == MaterialProfile::eye) {
        capture_rtv = data.material_data_rtv.Get();
        capture_shader = data.eye_mask_pixel_shader.Get();
        ID3D11Buffer* capture_buffer = data.capture_constants.Get();
        context->PSSetConstantBuffers(5, 1, &capture_buffer);
    } else if (material.profile == MaterialProfile::hair) {
        capture_rtv = data.material_data_rtv.Get();
        capture_shader = data.hair_capture_pixel_shader.Get();
    } else if (material.profile == MaterialProfile::foliage) {
        capture_rtv = data.material_data_rtv.Get();
        capture_shader = data.foliage_capture_pixel_shader.Get();
    }
    context->PSSetShader(capture_shader, nullptr, 0);
    context->OMSetRenderTargets(
        capture_rtv != nullptr ? 1u : 0u,
        capture_rtv != nullptr ? &capture_rtv : nullptr,
        data.mask_write_dsv.Get());
    const FLOAT zero_blend[4]{};
    context->OMSetBlendState(data.no_blend_state.Get(), zero_blend, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(
        data.mask_write_state.Get(), static_cast<UINT>(material.profile));
    if (instance_count == 1 && first_instance == 0) {
        context->DrawIndexed(index_count, first_index, vertex_offset);
    } else {
        context->DrawIndexedInstanced(
            index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    context->PSSetShader(
        pixel_shader, pixel_class_instances.data(), pixel_class_count);
    context->PSSetConstantBuffers(5, 1, &previous_capture_constants);
    context->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
    context->OMSetBlendState(blend_state, blend_factor, sample_mask);
    context->OMSetDepthStencilState(depth_state, stencil_reference);
    for (ID3D11RenderTargetView* view : rtvs) {
        if (view != nullptr) view->Release();
    }
    dsv->Release();
    if (pixel_shader != nullptr) pixel_shader->Release();
    for (UINT index = 0; index < pixel_class_count; ++index) {
        if (pixel_class_instances[index] != nullptr) {
            pixel_class_instances[index]->Release();
        }
    }
    if (blend_state != nullptr) blend_state->Release();
    if (depth_state != nullptr) depth_state->Release();
    if (previous_capture_constants != nullptr) {
        previous_capture_constants->Release();
    }
    ++data.mask_draw_counts[profile_index];
    return true;
}

bool UpdateConstants(
    ID3D11DeviceContext* context,
    DeviceData& data,
    MaterialProfile profile,
    float direction_x,
    float direction_y) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            data.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
        mapped.pData == nullptr) {
        return false;
    }
    SssConstants constants{};
    constants.inverse_resolution[0] = 1.0f / static_cast<float>(data.width);
    constants.inverse_resolution[1] = 1.0f / static_cast<float>(data.height);
    float radius_multiplier = 1.0f;
    float strength_multiplier = 1.0f;
    switch (profile) {
    case MaterialProfile::eye:
        radius_multiplier = 0.22f;
        strength_multiplier = 0.45f;
        break;
    case MaterialProfile::hair:
        radius_multiplier = 0.40f;
        strength_multiplier = 0.65f;
        break;
    case MaterialProfile::teeth:
        radius_multiplier = 0.30f;
        strength_multiplier = 0.50f;
        break;
    case MaterialProfile::foliage:
        radius_multiplier = 0.50f;
        strength_multiplier = 0.40f;
        constants.profile_anisotropy = 1.75f;
        break;
    default:
        break;
    }
    constants.radius = kJimenezBaseRadiusMetres * data.settings.radius_scale *
        radius_multiplier;
    constants.strength = data.settings.strength * strength_multiplier;
    constants.direction[0] = direction_x;
    constants.direction[1] = direction_y;
    constants.specular_scale = 1.0f;
    constants.material_profile = static_cast<std::uint32_t>(profile);
#if defined(SPATCH_SSS_DEVELOPMENT)
    constants.debug_view = static_cast<float>(
        g_debug_view.load(std::memory_order_relaxed));
#endif
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(data.constants.Get(), 0);
    return true;
}

bool IsExternalViewConstants(ID3D11Buffer* buffer) noexcept {
    if (buffer == nullptr) {
        return false;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    constexpr UINT kRequiredBytes = 5u * 4u * sizeof(float);
    return desc.ByteWidth >= kRequiredBytes &&
        (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

SssRunResult RunSubsurfaceScattering(
    ID3D11DeviceContext* context, DeviceData& data) {
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    const std::uint32_t scattering_draw_count = std::accumulate(
        data.mask_draw_counts.begin() + 1,
        data.mask_draw_counts.end(), 0u);
    if (data.mask_frame != frame || scattering_draw_count == 0 ||
        !g_native_hooks_ready.load()) {
        data.consecutive_input_failures = 0;
        return SssRunResult::skipped;
    }
    if (data.processed_frame == frame) {
        return SssRunResult::skipped;
    }
#if defined(SPATCH_SSS_DEVELOPMENT)
    if (!g_development_enabled.load(std::memory_order_relaxed)) {
        data.consecutive_input_failures = 0;
        return SssRunResult::skipped;
    }
#endif

    std::array<ID3D11ShaderResourceView*, 9> game_srvs{};
    context->PSGetShaderResources(
        0, static_cast<UINT>(game_srvs.size()), game_srvs.data());
    ComPtr<ID3D11Resource> lighting_resource;
    ComPtr<ID3D11Texture2D> lighting_texture;
    if (game_srvs[4] != nullptr) {
        game_srvs[4]->GetResource(lighting_resource.GetAddressOf());
        if (lighting_resource) {
            lighting_resource.As(&lighting_texture);
        }
    }
    ID3D11Buffer* external_view_constants = nullptr;
    context->VSGetConstantBuffers(1, 1, &external_view_constants);
    ID3D11DepthStencilView* scene_depth_view = nullptr;
    context->OMGetRenderTargets(0, nullptr, &scene_depth_view);

    const bool inputs_valid = lighting_texture &&
        IsExternalViewConstants(external_view_constants) &&
        CreateSceneDepthView(data, scene_depth_view) &&
        CreateLightingResources(data, lighting_texture.Get());
    if (scene_depth_view != nullptr) scene_depth_view->Release();
    if (!inputs_valid) {
        for (ID3D11ShaderResourceView* view : game_srvs) {
            if (view != nullptr) view->Release();
        }
        if (external_view_constants != nullptr) external_view_constants->Release();
        return SssRunResult::input_unavailable;
    }

#if defined(SPATCH_SSS_DEVELOPMENT)
    GpuTimingScope gpu_timing(context, data);
#endif
    SavedGraphicsState saved(context);
    std::array<ID3D11ShaderResourceView*, 9> null_srvs{};
    context->PSSetShaderResources(
        0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    context->OMSetRenderTargets(0, nullptr, nullptr);
    // Preserve one immutable native baseline for the entire transaction. Each
    // profile owns a disjoint stencil value, so all profiles can read this
    // baseline while successful earlier profile writes remain untouched.
    context->CopyResource(data.original_lighting.Get(), lighting_texture.Get());

    const auto release_inputs = [&game_srvs, external_view_constants]() noexcept {
        for (ID3D11ShaderResourceView* view : game_srvs) {
            if (view != nullptr) view->Release();
        }
        external_view_constants->Release();
    };
    const auto rollback = [&]() {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context->CopyResource(lighting_texture.Get(), data.original_lighting.Get());
        saved.Restore();
        release_inputs();
        return SssRunResult::input_unavailable;
    };

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(data.fullscreen_vertex_shader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(data.fullscreen_rasterizer_state.Get());
    const D3D11_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(data.width), static_cast<float>(data.height),
        0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{
        0, 0, static_cast<LONG>(data.width), static_cast<LONG>(data.height)};
    context->RSSetScissorRects(1, &scissor);
    const FLOAT blend_factor[4]{};
    context->OMSetBlendState(data.no_blend_state.Get(), blend_factor, 0xFFFFFFFFu);
    ID3D11SamplerState* sampler = data.linear_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* constant_buffers[] = {
        data.constants.Get(), external_view_constants};
    context->PSSetConstantBuffers(0, 2, constant_buffers);

    constexpr FLOAT kTransparentBlack[4]{};
    bool applied_profile = false;
    for (std::uint32_t value = static_cast<std::uint32_t>(MaterialProfile::skin);
         value <= static_cast<std::uint32_t>(MaterialProfile::teeth);
         ++value) {
        const MaterialProfile profile = static_cast<MaterialProfile>(value);
        if (data.mask_draw_counts[value] == 0 ||
            !IsProfileEnabled(data.settings, profile)) {
            continue;
        }

        // Profiles use the immutable native baseline. The vertical pass writes
        // only pixels whose stencil equals this profile, so earlier profile
        // results remain untouched and cross-material blur is impossible.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context->ClearRenderTargetView(
            data.temporary_lighting_rtv.Get(), kTransparentBlack);
        context->OMSetDepthStencilState(
            data.blur_stencil_state.Get(), static_cast<UINT>(profile));

        if (!UpdateConstants(context, data, profile, 1.0f, 0.0f)) {
            return rollback();
        }
        ID3D11ShaderResourceView* horizontal_srvs[] = {
            data.original_lighting_srv.Get(),
            data.original_lighting_srv.Get(),
            data.scene_depth_srv.Get(),
            data.mask_depth_srv.Get(),
            data.mask_stencil_srv.Get(),
            data.material_data_srv.Get()};
        context->PSSetShaderResources(0, 6, horizontal_srvs);
        ID3D11RenderTargetView* horizontal_target =
            data.temporary_lighting_rtv.Get();
        context->OMSetRenderTargets(
            1, &horizontal_target, data.mask_read_dsv.Get());
        context->PSSetShader(data.horizontal_pixel_shader.Get(), nullptr, 0);
        context->Draw(3, 0);

        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        if (!UpdateConstants(context, data, profile, 0.0f, 1.0f)) {
            return rollback();
        }
        ID3D11ShaderResourceView* vertical_srvs[] = {
            data.temporary_lighting_srv.Get(),
            data.original_lighting_srv.Get(),
            data.scene_depth_srv.Get(),
            data.mask_depth_srv.Get(),
            data.mask_stencil_srv.Get(),
            data.material_data_srv.Get()};
        context->PSSetShaderResources(0, 6, vertical_srvs);
        ID3D11RenderTargetView* vertical_target =
            data.output_lighting_rtv.Get();
        context->OMSetRenderTargets(
            1, &vertical_target, data.mask_read_dsv.Get());
        context->PSSetShader(data.vertical_pixel_shader.Get(), nullptr, 0);
        context->Draw(3, 0);
        applied_profile = true;
    }

    const std::size_t foliage_index =
        static_cast<std::size_t>(MaterialProfile::foliage);
    if (data.mask_draw_counts[foliage_index] != 0 &&
        IsProfileEnabled(data.settings, MaterialProfile::foliage)) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context->OMSetDepthStencilState(
            data.blur_stencil_state.Get(),
            static_cast<UINT>(MaterialProfile::foliage));
        if (!UpdateConstants(
                context, data, MaterialProfile::foliage, 1.0f, 0.0f)) {
            return rollback();
        }
        ID3D11ShaderResourceView* foliage_srvs[] = {
            data.original_lighting_srv.Get(),
            data.original_lighting_srv.Get(),
            data.scene_depth_srv.Get(),
            data.mask_depth_srv.Get(),
            data.mask_stencil_srv.Get(),
            data.material_data_srv.Get()};
        context->PSSetShaderResources(0, 6, foliage_srvs);
        ID3D11RenderTargetView* foliage_target =
            data.output_lighting_rtv.Get();
        context->OMSetRenderTargets(
            1, &foliage_target, data.mask_read_dsv.Get());
        context->PSSetShader(
            data.foliage_transmission_pixel_shader.Get(), nullptr, 0);
        context->Draw(3, 0);
        applied_profile = true;
    }

    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->PSSetShaderResources(
        0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    saved.Restore();

    release_inputs();
    if (!applied_profile) {
        return SssRunResult::skipped;
    }
    data.processed_frame = frame;
    data.consecutive_input_failures = 0;
    ++data.applied_frames;
    if (!data.logged_active) {
        data.logged_active = true;
        Log(reshade::log::level::info,
            "[ShenLong-SSS] dispatched at %ux%u; quality=%d, "
            "replayed_draws={skin:%u eye:%u hair:%u teeth:%u foliage:%u}.",
            data.width, data.height, data.settings.quality,
            data.mask_draw_counts[static_cast<std::size_t>(MaterialProfile::skin)],
            data.mask_draw_counts[static_cast<std::size_t>(MaterialProfile::eye)],
            data.mask_draw_counts[static_cast<std::size_t>(MaterialProfile::hair)],
            data.mask_draw_counts[static_cast<std::size_t>(MaterialProfile::teeth)],
            data.mask_draw_counts[foliage_index]);
    }
    return SssRunResult::applied;
}

bool IsGBufferBinding(
    std::uint32_t count,
    const reshade::api::resource_view* render_targets,
    reshade::api::resource_view depth_stencil) noexcept {
    if (count < 3 || render_targets == nullptr || depth_stencil.handle == 0) {
        return false;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::uint32_t index = 0; index < 3; ++index) {
        if (render_targets[index].handle == 0) {
            return false;
        }
        auto* view = NativePointer<ID3D11RenderTargetView>(render_targets[index].handle);
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        view->GetResource(resource.GetAddressOf());
        if (!resource || FAILED(resource.As(&texture))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            desc.SampleDesc.Count != 1 || desc.MipLevels != 1 ||
            desc.ArraySize != 1 ||
            (index != 0 && (desc.Width != width || desc.Height != height))) {
            return false;
        }
        width = desc.Width;
        height = desc.Height;
    }
    auto* dsv = NativePointer<ID3D11DepthStencilView>(depth_stencil.handle);
    ComPtr<ID3D11Resource> depth_resource;
    ComPtr<ID3D11Texture2D> depth_texture;
    dsv->GetResource(depth_resource.GetAddressOf());
    if (!depth_resource || FAILED(depth_resource.As(&depth_texture))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_texture->GetDesc(&depth_desc);
    return depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS &&
        depth_desc.SampleDesc.Count == 1 && depth_desc.MipLevels == 1 &&
        depth_desc.ArraySize == 1 && depth_desc.Width == width &&
        depth_desc.Height == height;
}

bool IsDeviceRuntimeReady(const DeviceData* data) noexcept {
    return data != nullptr && data->runtime_registered &&
        data->settings.enabled && data->ready &&
        g_native_hooks_ready.load(std::memory_order_acquire);
}

void RegisterReadyDevice(DeviceData& data) noexcept {
    std::scoped_lock lifecycle_lock(g_device_lifecycle_mutex);
    if (data.runtime_registered || !data.settings.enabled || !data.ready ||
        !g_native_hooks_ready.load(std::memory_order_acquire)) {
        return;
    }
    const bool first_device =
        g_ready_device_count.load(std::memory_order_acquire) == 0;
    if (first_device) {
        g_native_hook_accepting.store(false, std::memory_order_release);
        if (DrainAcceptedNativeHookCalls()) {
            std::unique_lock ranges_lock(g_skin_ranges_mutex);
            g_skin_ranges.clear();
        }
    }
    data.runtime_registered = true;
    g_ready_device_count.fetch_add(1, std::memory_order_acq_rel);
    g_native_hook_accepting.store(true, std::memory_order_release);
}

void UnregisterReadyDevice(DeviceData& data) noexcept {
    std::scoped_lock lifecycle_lock(g_device_lifecycle_mutex);
    if (!data.runtime_registered) {
        return;
    }
    data.runtime_registered = false;
    std::uint32_t count = g_ready_device_count.load(std::memory_order_acquire);
    while (count != 0 && !g_ready_device_count.compare_exchange_weak(
                             count, count - 1,
                             std::memory_order_acq_rel,
                              std::memory_order_acquire)) {
    }
    if (count == 1) {
        g_native_hook_accepting.store(false, std::memory_order_release);
        if (DrainAcceptedNativeHookCalls()) {
            std::unique_lock ranges_lock(g_skin_ranges_mutex);
            g_skin_ranges.clear();
        } else {
            Log(reshade::log::level::warning,
                "[ShenLong-SSS] device teardown capture drain timed out; stale ranges are retained but capture remains disabled.");
        }
    }
}

void OnInitDevice(reshade::api::device* device) {
    DeviceData* data =
        spatch::graphics::detail::CreatePrivateData<DeviceData>(device);
    if (data == nullptr) {
        return;
    }
    data->settings = LoadSettings();
    if (!data->settings.enabled) {
        return;
    }
    if (device->get_api() != reshade::api::device_api::d3d11) {
        Log(reshade::log::level::warning,
            "[ShenLong-SSS] unsupported graphics API; SSS disabled.");
        return;
    }
    data->native_device = NativePointer<ID3D11Device>(device->get_native());
    data->ready = data->native_device != nullptr &&
        InitializeShadersAndStates(data->native_device, *data);
    if (!data->ready) {
        Log(reshade::log::level::error,
            "[ShenLong-SSS] initialization failed; rendering is unchanged.");
        return;
    }
    std::call_once(g_native_hooks_once, [] {
        const bool installed = InstallNativeHooks();
        Log(installed ? reshade::log::level::info : reshade::log::level::error,
            installed
                ? "[ShenLong-SSS] exact material-profile hooks active."
                : "[ShenLong-SSS] native hook installation failed; SSS disabled.");
    });
    RegisterReadyDevice(*data);
}

void OnDestroyDevice(reshade::api::device* device) {
    if (DeviceData* data = device->get_private_data<DeviceData>()) {
        UnregisterReadyDevice(*data);
    }
    device->destroy_private_data<DeviceData>();
}

void OnInitCommandList(reshade::api::command_list* command_list) {
    spatch::graphics::detail::CreatePrivateData<CommandListData>(command_list);
}

void OnDestroyCommandList(reshade::api::command_list* command_list) {
    command_list->destroy_private_data<CommandListData>();
}

__declspec(noinline) bool QueryResourceDimensionSafely(
    ID3D11Resource* resource, D3D11_RESOURCE_DIMENSION& dimension) noexcept {
    if (resource == nullptr) {
        return false;
    }
    __try {
        resource->GetType(&dimension);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnDestroyResource(
    reshade::api::device*,
    reshade::api::resource resource) {
    if (resource.handle == 0) {
        return;
    }
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (!QueryResourceDimensionSafely(
            NativePointer<ID3D11Resource>(resource.handle), dimension)) {
        return;
    }
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return;
    }
    const std::uintptr_t native_resource =
        static_cast<std::uintptr_t>(resource.handle);
    try {
        std::unique_lock lock(g_skin_ranges_mutex);
        g_skin_ranges.erase(native_resource);
    } catch (...) {
    }
}

void OnResetCommandList(reshade::api::command_list* command_list) {
    if (CommandListData* data = command_list->get_private_data<CommandListData>()) {
        *data = {};
    }
}

template <std::size_t Size>
const ShaderIdentity* MatchShaderIdentity(
    const reshade::api::shader_desc& description,
    const std::array<ShaderIdentity, Size>& identities) noexcept {
    if (description.code == nullptr || description.code_size < 20) {
        return nullptr;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(description.code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return nullptr;
    }
    for (const ShaderIdentity& identity : identities) {
        if (description.code_size != identity.bytecode_size ||
            std::memcmp(
                bytes + 4, identity.dxbc_checksum.data(),
                identity.dxbc_checksum.size()) != 0) {
            continue;
        }
        return compute_crc32(bytes, description.code_size) == identity.crc32
            ? &identity
            : nullptr;
    }
    return nullptr;
}

void OnInitPipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline) {
    if (device->get_api() != reshade::api::device_api::d3d11 ||
        pipeline.handle == 0 || subobjects == nullptr) {
        return;
    }
    DeviceData* data = device->get_private_data<DeviceData>();
    if (!IsDeviceRuntimeReady(data)) {
        return;
    }
    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto& subobject = subobjects[index];
        const bool is_pixel_shader =
            subobject.type == reshade::api::pipeline_subobject_type::pixel_shader;
        const bool is_vertex_shader =
            subobject.type == reshade::api::pipeline_subobject_type::vertex_shader;
        if ((!is_pixel_shader && !is_vertex_shader) ||
            subobject.count == 0 || subobject.data == nullptr) {
            continue;
        }
        const auto* descriptions =
            static_cast<const reshade::api::shader_desc*>(subobject.data);
        for (std::uint32_t shader_index = 0;
             shader_index < subobject.count;
             ++shader_index) {
            const auto& description = descriptions[shader_index];
            if (is_pixel_shader) {
                const ShaderIdentity* identity = MatchShaderIdentity(
                    description, kTrackedPixelShaders);
                if (identity == nullptr) {
                    continue;
                }
                NativePointer<ID3D11PixelShader>(pipeline.handle)->SetPrivateData(
                    kPixelShaderHashTag, sizeof(identity->crc32), &identity->crc32);
                return;
            }
            const ShaderIdentity* identity = MatchShaderIdentity(
                description, kTrackedVertexShaders);
            if (identity == nullptr) {
                continue;
            }
            NativePointer<ID3D11VertexShader>(pipeline.handle)->SetPrivateData(
                kVertexShaderHashTag, sizeof(identity->crc32), &identity->crc32);
            return;
        }
    }
}

void OnBindPipeline(
    reshade::api::command_list* command_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline pipeline) {
    if (g_inside_sss) {
        return;
    }
    const DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!IsDeviceRuntimeReady(device_data)) {
        return;
    }
    CommandListData* data = command_list->get_private_data<CommandListData>();
    if (data == nullptr) {
        return;
    }
    if ((stages & reshade::api::pipeline_stage::pixel_shader) != 0) {
        data->pixel_shader_hash = 0;
        if (pipeline.handle != 0) {
            UINT size = sizeof(data->pixel_shader_hash);
            NativePointer<ID3D11PixelShader>(pipeline.handle)->GetPrivateData(
                kPixelShaderHashTag, &size, &data->pixel_shader_hash);
        }
    }
    if ((stages & reshade::api::pipeline_stage::vertex_shader) != 0) {
        data->vertex_shader_hash = 0;
        if (pipeline.handle != 0) {
            UINT size = sizeof(data->vertex_shader_hash);
            NativePointer<ID3D11VertexShader>(pipeline.handle)->GetPrivateData(
                kVertexShaderHashTag, &size, &data->vertex_shader_hash);
        }
    }
}

void OnBindRenderTargets(
    reshade::api::command_list* command_list,
    std::uint32_t count,
    const reshade::api::resource_view* render_targets,
    reshade::api::resource_view depth_stencil) {
    if (g_inside_sss) {
        return;
    }
    CommandListData* data = command_list->get_private_data<CommandListData>();
    if (data == nullptr) {
        return;
    }
    data->render_targets = {};
    data->depth_stencil = {};
    data->render_target_count = 0;
    data->gbuffer_binding = CommandListData::GBufferBinding::unavailable;
    DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (!IsDeviceRuntimeReady(device_data)) {
        return;
    }
    if (count < data->render_targets.size() || render_targets == nullptr ||
        depth_stencil.handle == 0) {
        return;
    }
    for (std::size_t index = 0; index < data->render_targets.size(); ++index) {
        if (render_targets[index].handle == 0) {
            return;
        }
        data->render_targets[index] = render_targets[index];
    }
    data->depth_stencil = depth_stencil;
    data->render_target_count = count;
    data->gbuffer_binding = CommandListData::GBufferBinding::unvalidated;
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) {
    if (g_inside_sss || index_count == 0 || instance_count == 0) {
        return false;
    }
    CommandListData* command_data =
        command_list->get_private_data<CommandListData>();
    DeviceData* device_data =
        command_list->get_device()->get_private_data<DeviceData>();
    if (command_data == nullptr || !IsDeviceRuntimeReady(device_data)) {
        return false;
    }

    const bool is_final_composition =
        command_data->pixel_shader_hash == kFinalCompositionPixelShaderHash;
    const bool is_material_candidate =
        IsTrackedGBufferVertexShader(command_data->vertex_shader_hash) &&
        command_data->gbuffer_binding !=
            CommandListData::GBufferBinding::unavailable;
    if (!is_final_composition && !is_material_candidate) {
        return false;
    }

    auto* context = NativePointer<ID3D11DeviceContext>(command_list->get_native());
    if (context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }

    if (is_final_composition) {
        g_inside_sss = true;
        const SssRunResult result =
            RunSubsurfaceScattering(context, *device_data);
        g_inside_sss = false;
        if (result == SssRunResult::input_unavailable) {
            constexpr std::uint32_t kFailureLogThreshold = 120;
            if (device_data->consecutive_input_failures <
                kFailureLogThreshold) {
                ++device_data->consecutive_input_failures;
            }
            if (device_data->consecutive_input_failures ==
                    kFailureLogThreshold &&
                !device_data->logged_input_failure) {
                device_data->logged_input_failure = true;
                Log(reshade::log::level::warning,
                    "[ShenLong-SSS] required HDR inputs were unavailable for "
                    "120 consecutive attempts; affected frames were unchanged.");
            }
        }
        return false;
    }

    if (command_data->gbuffer_binding ==
        CommandListData::GBufferBinding::unvalidated) {
        command_data->gbuffer_binding = IsGBufferBinding(
            command_data->render_target_count,
            command_data->render_targets.data(),
            command_data->depth_stencil)
            ? CommandListData::GBufferBinding::valid
            : CommandListData::GBufferBinding::unavailable;
    }
    if (command_data->gbuffer_binding != CommandListData::GBufferBinding::valid) {
        return false;
    }
    if (!g_native_hooks_ready.load(std::memory_order_acquire)) {
        return false;
    }
    ID3D11Buffer* index_buffer = nullptr;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    context->IAGetIndexBuffer(&index_buffer, &index_format, &index_offset);
    const CapturedMaterial material = FindCapturedMaterial(
        index_buffer, index_format, index_offset, first_index, index_count);
    if (index_buffer != nullptr) {
        index_buffer->Release();
    }
    if (!material || !IsProfileEnabled(device_data->settings, material.profile) ||
        !IsExactProfileShaderBinding(
            material.profile,
            command_data->pixel_shader_hash,
            command_data->vertex_shader_hash)) {
        return false;
    }
    CaptureConstants capture_constants{};
    const CaptureConstants* capture_constants_pointer = nullptr;
    if (material.profile == MaterialProfile::eye) {
        if (!BuildEyeCaptureConstants(
                material.material_uid, capture_constants)) {
            return false;
        }
        capture_constants_pointer = &capture_constants;
    }
    g_inside_sss = true;
    WriteMaterialRangeToMask(
        context, *device_data, material, capture_constants_pointer,
        index_count, instance_count, first_index, vertex_offset,
        first_instance);
    g_inside_sss = false;
    return false;
}

void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain*,
    const reshade::api::rect*,
    const reshade::api::rect*,
    std::uint32_t,
    const reshade::api::rect*) {
    const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    PruneSkinRanges(frame);
#if defined(SPATCH_SSS_DEVELOPMENT)
    const bool f6_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool f7_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f6_down && !g_f6_was_down) {
        const int value =
            (g_debug_view.load(std::memory_order_relaxed) + 1) %
            static_cast<int>(kDebugViewNames.size());
        g_debug_view.store(value, std::memory_order_relaxed);
        Log(reshade::log::level::info,
            "[ShenLong-SSS] development view=%d (%s).",
            value, kDebugViewNames[static_cast<std::size_t>(value)]);
    }
    if (f7_down && !g_f7_was_down) {
        const bool value = !g_development_enabled.load(std::memory_order_relaxed);
        g_development_enabled.store(value, std::memory_order_relaxed);
        Log(reshade::log::level::info,
            "[ShenLong-SSS] development effect enabled=%d.", value ? 1 : 0);
    }
    g_f6_was_down = f6_down;
    g_f7_was_down = f7_down;
#endif
}

bool DrainAcceptedNativeHookCalls() noexcept {
    constexpr ULONGLONG kDrainTimeoutMilliseconds = 5000;
    const ULONGLONG deadline = GetTickCount64() + kDrainTimeoutMilliseconds;
    while (g_native_hook_accepted_calls.load(std::memory_order_acquire) != 0) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        if (!SwitchToThread()) {
            Sleep(1);
        }
    }
    return true;
}

void DeactivateNativeHooks(bool process_terminating) noexcept {
    g_native_hooks_ready.store(false, std::memory_order_release);
    g_native_hook_accepting.store(false, std::memory_order_release);

    if (process_terminating) {
        return;
    }

    std::scoped_lock lifecycle_lock(g_device_lifecycle_mutex);

    const bool drained = DrainAcceptedNativeHookCalls();
    if (drained) {
        std::unique_lock lock(g_skin_ranges_mutex);
        g_skin_ranges.clear();
    } else {
        Log(reshade::log::level::warning,
            "[ShenLong-SSS] native capture drain timed out; captured ranges and "
            "transparent hooks remain pinned for process lifetime.");
    }

    if (g_render_hook_installed.load(std::memory_order_acquire) ||
        g_alternate_hook_installed.load(std::memory_order_acquire)) {
        Log(reshade::log::level::info,
            "[ShenLong-SSS] native submit hooks retained transparent for process "
            "lifetime; no trampoline was freed.");
    }
}

}  // namespace

namespace spatch::graphics::sss {

void Attach(HMODULE module) {
    g_module = module;
    reshade::register_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::register_event<reshade::addon_event::destroy_resource>(
        GuardedCallback<OnDestroyResource>::Invoke);
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
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
        GuardedCallback<OnBindRenderTargets>::Invoke);
    reshade::register_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
    reshade::register_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
}

void Detach(bool process_terminating) noexcept {
    reshade::unregister_event<reshade::addon_event::present>(
        GuardedCallback<OnPresent>::Invoke);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(
        GuardedCallback<OnDrawIndexed>::Invoke);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
        GuardedCallback<OnBindRenderTargets>::Invoke);
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
    reshade::unregister_event<reshade::addon_event::destroy_resource>(
        GuardedCallback<OnDestroyResource>::Invoke);
    reshade::unregister_event<reshade::addon_event::destroy_device>(
        GuardedCallback<OnDestroyDevice>::Invoke);
    reshade::unregister_event<reshade::addon_event::init_device>(
        GuardedCallback<OnInitDevice>::Invoke);
    DeactivateNativeHooks(process_terminating);
    g_module = nullptr;
}

}  // namespace spatch::graphics::sss

#if !defined(SPATCH_GRAPHICS_UNIFIED)
extern "C" __declspec(dllexport) const char* NAME = "SPatch Subsurface Scattering";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Jimenez separable skin scattering for Sleeping Dogs: Definitive Edition";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) {
            return FALSE;
        }
        spatch::graphics::sss::Attach(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        spatch::graphics::sss::Detach(reserved != nullptr);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
#endif
