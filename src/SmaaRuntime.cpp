#include "SmaaRuntime.h"

#include "Logger.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"
#include "SmaaShaderSource.inl"

namespace spatch::smaa {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kPresentVtableIndex = 8;
constexpr UINT kResizeBuffersVtableIndex = 13;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present_original = nullptr;
ResizeBuffersFn g_resize_buffers_original = nullptr;

std::uintptr_t g_device_slot = 0;
std::uintptr_t g_context_slot = 0;
std::uintptr_t g_swapchain_slot = 0;
std::uintptr_t g_present_rtv_slot = 0;

std::atomic<bool> g_enabled = true;
std::atomic<bool> g_debug_keys_enabled = true;
std::atomic<bool> g_hooks_installed = false;
std::atomic<unsigned long long> g_present_count = 0;
std::atomic<unsigned long long> g_apply_count = 0;
std::atomic<unsigned long long> g_fail_count = 0;
std::atomic<unsigned long long> g_resize_count = 0;
std::atomic<unsigned int> g_width = 0;
std::atomic<unsigned int> g_height = 0;
std::atomic<bool> g_resources_ready = false;
std::atomic<bool> g_sized_resources_ready = false;
std::atomic<int> g_preset = 2;

std::mutex g_mutex;

// blendFactor removed: it was declared in the cbuffer but never read by any
// shader pass. The struct is now 32 bytes, still a valid cbuffer size.
struct ConstantBufferData {
    float rt_metrics[4];
    float subsample_indices[4];
};

struct PipelineState {
    ComPtr<ID3D11VertexShader> edge_vs;
    ComPtr<ID3D11PixelShader> edge_ps;
    ComPtr<ID3D11VertexShader> blend_vs;
    ComPtr<ID3D11PixelShader> blend_ps;
    ComPtr<ID3D11VertexShader> neighborhood_vs;
    ComPtr<ID3D11PixelShader> neighborhood_ps;
    ComPtr<ID3D11Buffer> constant_buffer;
    ComPtr<ID3D11BlendState> blend_state;
    ComPtr<ID3D11DepthStencilState> depth_state;
    ComPtr<ID3D11RasterizerState> rasterizer_state;
    // SMAA requires explicit linear+clamp and point+clamp samplers. The inline
    // SamplerState initialisers in the SMAA HLSL source are silently ignored by
    // D3DCompile outside of the FX framework, so we create and bind them here.
    ComPtr<ID3D11SamplerState> linear_sampler; // s0 – LinearSampler
    ComPtr<ID3D11SamplerState> point_sampler;  // s1 – PointSampler
    ComPtr<ID3D11Texture2D> area_texture;
    ComPtr<ID3D11ShaderResourceView> area_srv;
    ComPtr<ID3D11Texture2D> search_texture;
    ComPtr<ID3D11ShaderResourceView> search_srv;
    ComPtr<ID3D11Texture2D> source_texture;
    ComPtr<ID3D11ShaderResourceView> source_srv;
    ComPtr<ID3D11Texture2D> edges_texture;
    ComPtr<ID3D11RenderTargetView> edges_rtv;
    ComPtr<ID3D11ShaderResourceView> edges_srv;
    ComPtr<ID3D11Texture2D> blend_texture;
    ComPtr<ID3D11RenderTargetView> blend_rtv;
    ComPtr<ID3D11ShaderResourceView> blend_srv;
    DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    bool shaders_ready = false;
};

PipelineState g_pipeline;

thread_local bool g_present_reentry = false;

const char* GetPresetDefine() {
    switch (g_preset.load()) {
        case 0:
            return "#define SMAA_PRESET_LOW 1\n";
        case 1:
            return "#define SMAA_PRESET_MEDIUM 1\n";
        case 3:
            return "#define SMAA_PRESET_ULTRA 1\n";
        case 2:
        default:
            return "#define SMAA_PRESET_HIGH 1\n";
    }
}

std::string BuildShaderSource() {
    // Removed from cbuffer: blendFactor/padding (never read by any shader).
    // Removed from texture registers: colorTexGamma (t1), depthTex (t2),
    // velocityTex (t3) — all declared but unreferenced. Registers t4-t7 are
    // unchanged so the SRV binding arrays in ApplySmaa remain valid.
    static const char* kHeader = R"SMAAHLSL(
cbuffer SMAAConstants : register(b0) {
    float4 rtMetrics;
    float4 subsampleIndices;
};

#define SMAA_RT_METRICS rtMetrics
#define SMAA_HLSL_4_1 1
)SMAAHLSL";

    static const char* kFooter = R"SMAAHLSL(
Texture2D colorTex   : register(t0);
Texture2D edgesTex   : register(t4);
Texture2D blendTex   : register(t5);
Texture2D areaTex    : register(t6);
Texture2D searchTex  : register(t7);

float2 FullscreenUV(uint vertex_id) {
    return float2((vertex_id << 1) & 2, vertex_id & 2);
}

float4 FullscreenPosition(float2 uv) {
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

struct EdgeVsOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
};

struct BlendVsOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset[3] : TEXCOORD2;
};

struct NeighborhoodVsOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

EdgeVsOut EdgeVS(uint vertex_id : SV_VertexID) {
    EdgeVsOut output;
    output.texcoord = FullscreenUV(vertex_id);
    output.position = FullscreenPosition(output.texcoord);
    SMAAEdgeDetectionVS(output.texcoord, output.offset);
    return output;
}

float2 EdgePS(EdgeVsOut input) : SV_TARGET {
    return SMAAColorEdgeDetectionPS(input.texcoord, input.offset, colorTex);
}

BlendVsOut BlendVS(uint vertex_id : SV_VertexID) {
    BlendVsOut output;
    output.texcoord = FullscreenUV(vertex_id);
    output.position = FullscreenPosition(output.texcoord);
    SMAABlendingWeightCalculationVS(output.texcoord, output.pixcoord, output.offset);
    return output;
}

float4 BlendPS(BlendVsOut input) : SV_TARGET {
    return SMAABlendingWeightCalculationPS(
        input.texcoord,
        input.pixcoord,
        input.offset,
        edgesTex,
        areaTex,
        searchTex,
        subsampleIndices);
}

NeighborhoodVsOut NeighborhoodVS(uint vertex_id : SV_VertexID) {
    NeighborhoodVsOut output;
    output.texcoord = FullscreenUV(vertex_id);
    output.position = FullscreenPosition(output.texcoord);
    SMAANeighborhoodBlendingVS(output.texcoord, output.offset);
    return output;
}

float4 NeighborhoodPS(NeighborhoodVsOut input) : SV_TARGET {
    return SMAANeighborhoodBlendingPS(input.texcoord, input.offset, colorTex, blendTex);
}
    )SMAAHLSL";

    std::string source;
    std::size_t shader_size = 0;
    for (const std::string_view chunk : kSmaaShaderSourceChunks) {
        shader_size += chunk.size();
    }
    source.reserve(2048 + shader_size);
    source.append(kHeader);
    source.append(GetPresetDefine());
    for (const std::string_view chunk : kSmaaShaderSourceChunks) {
        source.append(chunk.data(), chunk.size());
    }
    source.append(kFooter);
    return source;
}

template <typename T>
T* ReadSlot(std::uintptr_t slot) {
    if (slot == 0) {
        return nullptr;
    }
    return *reinterpret_cast<T**>(slot);
}

bool IsOurSwapChain(IDXGISwapChain* swapchain) {
    IDXGISwapChain* const current = ReadSlot<IDXGISwapChain>(g_swapchain_slot);
    return current != nullptr && current == swapchain;
}

DXGI_FORMAT NormalizeTextureFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return format;
    }
}

bool CompileShader(const std::string& source,
                   const char* entry_point,
                   const char* profile,
                   ID3DBlob** shader_blob) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(),
                                  source.size(),
                                  "SPatch.SMAA",
                                  nullptr,
                                  nullptr,
                                  entry_point,
                                  profile,
                                  flags,
                                  0,
                                  shader_blob,
                                  errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors != nullptr) {
            log::ErrorF("smaa_compile_fail entry=%s profile=%s error=%s",
                        entry_point,
                        profile,
                        static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            log::ErrorF("smaa_compile_fail entry=%s profile=%s hr=0x%08X",
                        entry_point,
                        profile,
                        static_cast<unsigned int>(hr));
        }
        return false;
    }
    return true;
}

bool CreateLookupTexture(ID3D11Device* device,
                         DXGI_FORMAT format,
                         UINT width,
                         UINT height,
                         UINT row_pitch,
                         const void* data,
                         ID3D11Texture2D** texture,
                         ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    init.SysMemPitch = row_pitch;

    if (FAILED(device->CreateTexture2D(&desc, &init, texture))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    return SUCCEEDED(device->CreateShaderResourceView(*texture, &srv_desc, srv));
}

bool EnsureStaticResources(ID3D11Device* device) {
    if (g_pipeline.shaders_ready) {
        g_resources_ready.store(true);
        return true;
    }

    const std::string source = BuildShaderSource();

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;

    if (!CompileShader(source, "EdgeVS", "vs_5_0", vs_blob.GetAddressOf()) ||
        FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(),
                                          nullptr,
                                          g_pipeline.edge_vs.GetAddressOf()))) {
        return false;
    }

    if (!CompileShader(source, "EdgePS", "ps_5_0", ps_blob.GetAddressOf()) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(),
                                         nullptr,
                                         g_pipeline.edge_ps.GetAddressOf()))) {
        return false;
    }

    vs_blob.Reset();
    ps_blob.Reset();
    if (!CompileShader(source, "BlendVS", "vs_5_0", vs_blob.GetAddressOf()) ||
        FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(),
                                          nullptr,
                                          g_pipeline.blend_vs.GetAddressOf()))) {
        return false;
    }

    if (!CompileShader(source, "BlendPS", "ps_5_0", ps_blob.GetAddressOf()) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(),
                                         nullptr,
                                         g_pipeline.blend_ps.GetAddressOf()))) {
        return false;
    }

    vs_blob.Reset();
    ps_blob.Reset();
    if (!CompileShader(source, "NeighborhoodVS", "vs_5_0", vs_blob.GetAddressOf()) ||
        FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(),
                                          nullptr,
                                          g_pipeline.neighborhood_vs.GetAddressOf()))) {
        return false;
    }

    if (!CompileShader(source, "NeighborhoodPS", "ps_5_0", ps_blob.GetAddressOf()) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(),
                                         nullptr,
                                         g_pipeline.neighborhood_ps.GetAddressOf()))) {
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(ConstantBufferData);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb_desc, nullptr, g_pipeline.constant_buffer.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend_desc, g_pipeline.blend_state.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth_desc.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, g_pipeline.depth_state.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rast_desc{};
    rast_desc.FillMode = D3D11_FILL_SOLID;
    rast_desc.CullMode = D3D11_CULL_NONE;
    rast_desc.DepthClipEnable = FALSE;
    rast_desc.ScissorEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rast_desc, g_pipeline.rasterizer_state.GetAddressOf()))) {
        return false;
    }

    // LinearSampler (s0): matches SMAA's MIN_MAG_LINEAR_MIP_POINT + Clamp.
    // PointSampler (s1): matches SMAA's MIN_MAG_MIP_POINT + Clamp.
    // These are created explicitly because the inline SamplerState initialisers
    // in the embedded SMAA HLSL are ignored by D3DCompile outside of the FX
    // framework, leaving whatever sampler the game has bound at s0/s1 in effect.
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, g_pipeline.linear_sampler.GetAddressOf()))) {
            log::Error("smaa_linear_sampler_fail");
            return false;
        }
    }
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, g_pipeline.point_sampler.GetAddressOf()))) {
            log::Error("smaa_point_sampler_fail");
            return false;
        }
    }

    if (!CreateLookupTexture(device,
                             DXGI_FORMAT_R8G8_UNORM,
                             AREATEX_WIDTH,
                             AREATEX_HEIGHT,
                             AREATEX_PITCH,
                             areaTexBytes,
                             g_pipeline.area_texture.GetAddressOf(),
                             g_pipeline.area_srv.GetAddressOf())) {
        log::Error("smaa_area_texture_fail");
        return false;
    }

    if (!CreateLookupTexture(device,
                             DXGI_FORMAT_R8_UNORM,
                             SEARCHTEX_WIDTH,
                             SEARCHTEX_HEIGHT,
                             SEARCHTEX_PITCH,
                             searchTexBytes,
                             g_pipeline.search_texture.GetAddressOf(),
                             g_pipeline.search_srv.GetAddressOf())) {
        log::Error("smaa_search_texture_fail");
        return false;
    }

    g_pipeline.shaders_ready = true;
    g_resources_ready.store(true);
    log::Info("smaa_static_resources_ready");
    return true;
}

bool EnsureSizedResources(ID3D11Device* device, IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) {
        return false;
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC bb_desc{};
    backbuffer->GetDesc(&bb_desc);
    const DXGI_FORMAT source_format = NormalizeTextureFormat(bb_desc.Format);
    const UINT width = bb_desc.Width;
    const UINT height = bb_desc.Height;

    if (g_pipeline.width == width && g_pipeline.height == height &&
        g_pipeline.backbuffer_format == bb_desc.Format && g_pipeline.source_texture != nullptr &&
        g_pipeline.edges_texture != nullptr && g_pipeline.blend_texture != nullptr) {
        g_sized_resources_ready.store(true);
        g_width.store(width);
        g_height.store(height);
        return true;
    }

    g_pipeline.source_srv.Reset();
    g_pipeline.source_texture.Reset();
    g_pipeline.edges_rtv.Reset();
    g_pipeline.edges_srv.Reset();
    g_pipeline.edges_texture.Reset();
    g_pipeline.blend_rtv.Reset();
    g_pipeline.blend_srv.Reset();
    g_pipeline.blend_texture.Reset();

    D3D11_TEXTURE2D_DESC source_desc{};
    source_desc.Width = width;
    source_desc.Height = height;
    source_desc.MipLevels = 1;
    source_desc.ArraySize = 1;
    source_desc.Format = bb_desc.Format;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_DEFAULT;
    source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&source_desc, nullptr, g_pipeline.source_texture.GetAddressOf()))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC source_srv_desc{};
    source_srv_desc.Format = source_format;
    source_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    source_srv_desc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(
            g_pipeline.source_texture.Get(), &source_srv_desc, g_pipeline.source_srv.GetAddressOf()))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC edge_desc{};
    edge_desc.Width = width;
    edge_desc.Height = height;
    edge_desc.MipLevels = 1;
    edge_desc.ArraySize = 1;
    edge_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    edge_desc.SampleDesc.Count = 1;
    edge_desc.Usage = D3D11_USAGE_DEFAULT;
    edge_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&edge_desc, nullptr, g_pipeline.edges_texture.GetAddressOf())) ||
        FAILED(device->CreateRenderTargetView(
            g_pipeline.edges_texture.Get(), nullptr, g_pipeline.edges_rtv.GetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(
            g_pipeline.edges_texture.Get(), nullptr, g_pipeline.edges_srv.GetAddressOf()))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC blend_desc{};
    blend_desc.Width = width;
    blend_desc.Height = height;
    blend_desc.MipLevels = 1;
    blend_desc.ArraySize = 1;
    blend_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    blend_desc.SampleDesc.Count = 1;
    blend_desc.Usage = D3D11_USAGE_DEFAULT;
    blend_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&blend_desc, nullptr, g_pipeline.blend_texture.GetAddressOf())) ||
        FAILED(device->CreateRenderTargetView(
            g_pipeline.blend_texture.Get(), nullptr, g_pipeline.blend_rtv.GetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(
            g_pipeline.blend_texture.Get(), nullptr, g_pipeline.blend_srv.GetAddressOf()))) {
        return false;
    }

    g_pipeline.width = width;
    g_pipeline.height = height;
    g_pipeline.backbuffer_format = bb_desc.Format;
    g_sized_resources_ready.store(true);
    g_width.store(width);
    g_height.store(height);
    log::InfoF("smaa_resources_ready width=%u height=%u format=%u", width, height, bb_desc.Format);
    return true;
}

struct ScopedPipelineState {
    ID3D11DeviceContext* context = nullptr;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ComPtr<ID3D11BlendState> blend_state;
    FLOAT blend_factor[4]{};
    UINT sample_mask = 0;
    ComPtr<ID3D11DepthStencilState> depth_state;
    UINT stencil_ref = 0;
    ComPtr<ID3D11RasterizerState> rasterizer_state;
    ComPtr<ID3D11InputLayout> input_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::array<ComPtr<ID3D11Buffer>, 1> vs_cbs{};
    std::array<ComPtr<ID3D11Buffer>, 1> ps_cbs{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 8> ps_srvs{};
    // Samplers s0 and s1 are saved/restored so SMAA's bindings don't leak
    // back into the game's render pipeline after each Present.
    std::array<ComPtr<ID3D11SamplerState>, 2> ps_samplers{};

    explicit ScopedPipelineState(ID3D11DeviceContext* ctx) : context(ctx) {
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reinterpret_cast<ID3D11RenderTargetView**>(rtvs.data()), dsv.GetAddressOf());
        context->RSGetViewports(&viewport_count, viewports);
        context->OMGetBlendState(blend_state.GetAddressOf(), blend_factor, &sample_mask);
        context->OMGetDepthStencilState(depth_state.GetAddressOf(), &stencil_ref);
        context->RSGetState(rasterizer_state.GetAddressOf());
        context->IAGetInputLayout(input_layout.GetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(vs.GetAddressOf(), nullptr, nullptr);
        context->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
        context->VSGetConstantBuffers(0, 1, reinterpret_cast<ID3D11Buffer**>(vs_cbs.data()));
        context->PSGetConstantBuffers(0, 1, reinterpret_cast<ID3D11Buffer**>(ps_cbs.data()));
        context->PSGetShaderResources(0, 8, reinterpret_cast<ID3D11ShaderResourceView**>(ps_srvs.data()));
        context->PSGetSamplers(0, static_cast<UINT>(ps_samplers.size()),
                               reinterpret_cast<ID3D11SamplerState**>(ps_samplers.data()));
    }

    ~ScopedPipelineState() {
        context->OMSetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
            reinterpret_cast<ID3D11RenderTargetView* const*>(rtvs.data()),
            dsv.Get());
        context->RSSetViewports(viewport_count, viewports);
        context->OMSetBlendState(blend_state.Get(), blend_factor, sample_mask);
        context->OMSetDepthStencilState(depth_state.Get(), stencil_ref);
        context->RSSetState(rasterizer_state.Get());
        context->IASetInputLayout(input_layout.Get());
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* vs_cb = vs_cbs[0].Get();
        ID3D11Buffer* ps_cb = ps_cbs[0].Get();
        context->VSSetConstantBuffers(0, 1, &vs_cb);
        context->PSSetConstantBuffers(0, 1, &ps_cb);
        context->PSSetShaderResources(
            0, static_cast<UINT>(ps_srvs.size()), reinterpret_cast<ID3D11ShaderResourceView* const*>(ps_srvs.data()));
        context->PSSetSamplers(0, static_cast<UINT>(ps_samplers.size()),
                               reinterpret_cast<ID3D11SamplerState* const*>(ps_samplers.data()));
    }
};

void UpdateConstants(ID3D11DeviceContext* context, UINT width, UINT height) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_pipeline.constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }

    auto* constants = static_cast<ConstantBufferData*>(mapped.pData);
    constants->rt_metrics[0] = 1.0f / static_cast<float>(width);
    constants->rt_metrics[1] = 1.0f / static_cast<float>(height);
    constants->rt_metrics[2] = static_cast<float>(width);
    constants->rt_metrics[3] = static_cast<float>(height);
    constants->subsample_indices[0] = 0.0f;
    constants->subsample_indices[1] = 0.0f;
    constants->subsample_indices[2] = 0.0f;
    constants->subsample_indices[3] = 0.0f;
    context->Unmap(g_pipeline.constant_buffer.Get(), 0);
}

void PrepareCommonState(ID3D11DeviceContext* context, UINT width, UINT height) {
    const D3D11_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(g_pipeline.blend_state.Get(), nullptr, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(g_pipeline.depth_state.Get(), 0);
    context->RSSetState(g_pipeline.rasterizer_state.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* const cb = g_pipeline.constant_buffer.Get();
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetConstantBuffers(0, 1, &cb);

    // Bind SMAA's required samplers. s0 = LinearSampler (MIN_MAG_LINEAR_MIP_POINT,
    // Clamp), s1 = PointSampler (MIN_MAG_MIP_POINT, Clamp). These match the
    // SamplerState names the embedded SMAA HLSL references. Without explicit
    // bindings, the game's own samplers would be inherited, causing blur/wrap
    // artifacts across all three passes.
    ID3D11SamplerState* samplers[2] = {
        g_pipeline.linear_sampler.Get(),
        g_pipeline.point_sampler.Get()
    };
    context->PSSetSamplers(0, 2, samplers);
}

bool ApplySmaa(IDXGISwapChain* swapchain) {
    ID3D11Device* const device = ReadSlot<ID3D11Device>(g_device_slot);
    ID3D11DeviceContext* const context = ReadSlot<ID3D11DeviceContext>(g_context_slot);
    if (device == nullptr || context == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    if (!EnsureStaticResources(device) || !EnsureSizedResources(device, swapchain)) {
        return false;
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC bb_desc{};
    backbuffer->GetDesc(&bb_desc);

    // NormalizeTextureFormat is used here because ResolveSubresource requires a
    // non-typeless format. Passing bb_desc.Format directly would fail when the
    // swapchain uses a TYPELESS format (e.g. DXGI_FORMAT_R8G8B8A8_TYPELESS).
    const DXGI_FORMAT resolve_format = NormalizeTextureFormat(bb_desc.Format);

    if (bb_desc.SampleDesc.Count > 1) {
        context->ResolveSubresource(
            g_pipeline.source_texture.Get(), 0, backbuffer.Get(), 0, resolve_format);
    } else {
        context->CopyResource(g_pipeline.source_texture.Get(), backbuffer.Get());
    }

    ID3D11RenderTargetView* present_rtv = ReadSlot<ID3D11RenderTargetView>(g_present_rtv_slot);
    ComPtr<ID3D11RenderTargetView> fallback_rtv;
    if (present_rtv == nullptr) {
        if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, fallback_rtv.GetAddressOf()))) {
            return false;
        }
        present_rtv = fallback_rtv.Get();
    }

    ScopedPipelineState saved_state(context);
    UpdateConstants(context, bb_desc.Width, bb_desc.Height);
    PrepareCommonState(context, bb_desc.Width, bb_desc.Height);

    const float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(g_pipeline.edges_rtv.Get(), clear_rgba);
    context->ClearRenderTargetView(g_pipeline.blend_rtv.Get(), clear_rgba);

    // Pass 1 – Edge detection. Only colorTex (t0) is needed; the former t1
    // (colorTexGamma) slot has been removed as it was never read by EdgePS.
    {
        context->VSSetShader(g_pipeline.edge_vs.Get(), nullptr, 0);
        context->PSSetShader(g_pipeline.edge_ps.Get(), nullptr, 0);
        ID3D11RenderTargetView* const rtv = g_pipeline.edges_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srvs[8] = {
            g_pipeline.source_srv.Get(), nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr};
        context->PSSetShaderResources(0, 8, srvs);
        context->Draw(3, 0);
    }

    // Pass 2 – Blend weight calculation. edgesTex=t4, areaTex=t6, searchTex=t7.
    {
        context->VSSetShader(g_pipeline.blend_vs.Get(), nullptr, 0);
        context->PSSetShader(g_pipeline.blend_ps.Get(), nullptr, 0);
        ID3D11RenderTargetView* const rtv = g_pipeline.blend_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srvs[8] = {
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            g_pipeline.edges_srv.Get(),
            nullptr,
            g_pipeline.area_srv.Get(),
            g_pipeline.search_srv.Get()};
        context->PSSetShaderResources(0, 8, srvs);
        context->Draw(3, 0);
    }

    // Pass 3 – Neighborhood blending. colorTex=t0, blendTex=t5.
    {
        context->VSSetShader(g_pipeline.neighborhood_vs.Get(), nullptr, 0);
        context->PSSetShader(g_pipeline.neighborhood_ps.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, &present_rtv, nullptr);
        ID3D11ShaderResourceView* srvs[8] = {
            g_pipeline.source_srv.Get(), nullptr, nullptr, nullptr,
            nullptr, g_pipeline.blend_srv.Get(), nullptr, nullptr};
        context->PSSetShaderResources(0, 8, srvs);
        context->Draw(3, 0);
    }

    ID3D11ShaderResourceView* null_srvs[8] = {};
    context->PSSetShaderResources(0, 8, null_srvs);
    return true;
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags) {
    g_present_count.fetch_add(1);

    if (g_present_reentry) {
        return g_present_original(swapchain, sync_interval, flags);
    }
    g_present_reentry = true;

    if (g_enabled.load() && IsOurSwapChain(swapchain)) {
        if (ApplySmaa(swapchain)) {
            g_apply_count.fetch_add(1);
        } else {
            g_fail_count.fetch_add(1);
        }
    }

    g_present_reentry = false;
    return g_present_original(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* swapchain,
                                              UINT buffer_count,
                                              UINT width,
                                              UINT height,
                                              DXGI_FORMAT new_format,
                                              UINT swap_chain_flags) {
    g_resize_count.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pipeline.source_srv.Reset();
        g_pipeline.source_texture.Reset();
        g_pipeline.edges_rtv.Reset();
        g_pipeline.edges_srv.Reset();
        g_pipeline.edges_texture.Reset();
        g_pipeline.blend_rtv.Reset();
        g_pipeline.blend_srv.Reset();
        g_pipeline.blend_texture.Reset();
        g_pipeline.width = 0;
        g_pipeline.height = 0;
        g_pipeline.backbuffer_format = DXGI_FORMAT_UNKNOWN;
        g_sized_resources_ready.store(false);
        g_width.store(0);
        g_height.store(0);
    }

    return g_resize_buffers_original(
        swapchain, buffer_count, width, height, new_format, swap_chain_flags);
}

}  // namespace

bool Initialize(std::uintptr_t module_base,
                std::uintptr_t device_slot,
                std::uintptr_t context_slot,
                std::uintptr_t swapchain_slot,
                std::uintptr_t present_rtv_slot) {
    (void)module_base;
    g_device_slot = device_slot;
    g_context_slot = context_slot;
    g_swapchain_slot = swapchain_slot;
    g_present_rtv_slot = present_rtv_slot;
    return true;
}

void Shutdown() {
    if (g_hooks_installed.exchange(false)) {
        IDXGISwapChain* const swapchain = ReadSlot<IDXGISwapChain>(g_swapchain_slot);
        if (swapchain != nullptr) {
            void** const vtable = *reinterpret_cast<void***>(swapchain);
            if (vtable != nullptr) {
                MH_RemoveHook(vtable[kPresentVtableIndex]);
                MH_RemoveHook(vtable[kResizeBuffersVtableIndex]);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pipeline = {};
    g_resources_ready.store(false);
    g_sized_resources_ready.store(false);
    g_width.store(0);
    g_height.store(0);
}

void MaybeInstallHooks() {
    if (g_hooks_installed.load()) {
        return;
    }

    IDXGISwapChain* const swapchain = ReadSlot<IDXGISwapChain>(g_swapchain_slot);
    if (swapchain == nullptr) {
        return;
    }

    void** const vtable = *reinterpret_cast<void***>(swapchain);
    if (vtable == nullptr) {
        return;
    }

    if (MH_CreateHook(vtable[kPresentVtableIndex],
                      reinterpret_cast<void*>(&DetourPresent),
                      reinterpret_cast<void**>(&g_present_original)) != MH_OK) {
        log::Error("smaa_present_hook_create_fail");
        return;
    }

    if (MH_CreateHook(vtable[kResizeBuffersVtableIndex],
                      reinterpret_cast<void*>(&DetourResizeBuffers),
                      reinterpret_cast<void**>(&g_resize_buffers_original)) != MH_OK) {
        log::Error("smaa_resize_hook_create_fail");
        MH_RemoveHook(vtable[kPresentVtableIndex]);
        return;
    }

    g_hooks_installed.store(true);
    log::InfoF("smaa_hooks_installed present=0x%p resize=0x%p swapchain=0x%p",
               vtable[kPresentVtableIndex],
               vtable[kResizeBuffersVtableIndex],
               swapchain);
}

bool GetEnabled() {
    return g_enabled.load();
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled);
}

bool GetDebugKeysEnabled() {
    return g_debug_keys_enabled.load();
}

void SetDebugKeysEnabled(bool enabled) {
    g_debug_keys_enabled.store(enabled);
}

void SetPreset(int preset) {
    if (preset < 0) {
        preset = 0;
    } else if (preset > 3) {
        preset = 3;
    }

    const int previous = g_preset.exchange(preset);
    if (previous == preset) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pipeline.edge_vs.Reset();
    g_pipeline.edge_ps.Reset();
    g_pipeline.blend_vs.Reset();
    g_pipeline.blend_ps.Reset();
    g_pipeline.neighborhood_vs.Reset();
    g_pipeline.neighborhood_ps.Reset();
    g_pipeline.constant_buffer.Reset();
    g_pipeline.blend_state.Reset();
    g_pipeline.depth_state.Reset();
    g_pipeline.rasterizer_state.Reset();
    g_pipeline.linear_sampler.Reset();
    g_pipeline.point_sampler.Reset();
    g_pipeline.area_texture.Reset();
    g_pipeline.area_srv.Reset();
    g_pipeline.search_texture.Reset();
    g_pipeline.search_srv.Reset();
    g_pipeline.shaders_ready = false;
    g_resources_ready.store(false);
    g_sized_resources_ready.store(false);
    g_width.store(0);
    g_height.store(0);
}

Stats GetStats() {
    return Stats{
        g_present_count.load(),
        g_apply_count.load(),
        g_fail_count.load(),
        g_resize_count.load(),
        g_width.load(),
        g_height.load(),
        g_hooks_installed.load(),
        g_resources_ready.load() && g_sized_resources_ready.load(),
        g_enabled.load()};
}

}  // namespace spatch::smaa
