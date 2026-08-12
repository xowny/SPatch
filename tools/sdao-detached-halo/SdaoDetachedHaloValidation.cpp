#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 64;
constexpr UINT kLogicalLayerCount = 2;
constexpr UINT kPackedSliceCount = (kLogicalLayerCount + 1u) / 2u;
constexpr float kProjectionB = 0.330025941f;
constexpr float kProjectionA = 1.00007856f;
constexpr float kWallDepth = 4.4f;
constexpr float kForegroundDepth = 4.2f;
constexpr float kHiddenFrontDepth = 3.0f;
constexpr float kHiddenShellDepth = 4.2f;
constexpr UINT kForegroundLeft = 24;
constexpr UINT kForegroundRight = 32;
constexpr UINT kForegroundTop = 12;
constexpr UINT kForegroundBottom = 52;

struct Float4 {
    float x;
    float y;
    float z;
    float w;
};

struct SdaoConstants {
    float radius;
    float strength;
    float falloff_range;
    float radius_multiplier;
};

struct UInt2 {
    std::uint32_t x;
    std::uint32_t y;
};

static_assert(sizeof(Float4) == 16);
static_assert(sizeof(SdaoConstants) == 16);
static_assert(sizeof(UInt2) == 8);

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[256]{};
        sprintf_s(message, "%s failed with HRESULT 0x%08X", operation,
                  static_cast<unsigned int>(result));
        Fail(message);
    }
}

void WriteBytes(const std::filesystem::path& path, const void* data,
                std::size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        Fail("Could not create output file: " + path.string());
    }
    stream.write(static_cast<const char*>(data),
                 static_cast<std::streamsize>(size));
    if (!stream) {
        Fail("Could not write output file: " + path.string());
    }
}

ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& shader_path,
                              const char* entry_point,
                              const D3D_SHADER_MACRO* macros) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                           D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const HRESULT result = D3DCompileFromFile(
        shader_path.c_str(), macros, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry_point, "cs_5_0", flags, 0, bytecode.GetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(result) || !bytecode) {
        const char* detail = errors && errors->GetBufferPointer()
                                 ? static_cast<const char*>(errors->GetBufferPointer())
                                 : "no compiler diagnostics";
        Fail(std::string("D3DCompileFromFile(") + entry_point +
             ") failed: " + detail);
    }
    return bytecode;
}

ComPtr<ID3D11ComputeShader> CreateShader(ID3D11Device* device,
                                        ID3DBlob* bytecode,
                                        const char* operation) {
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(bytecode->GetBufferPointer(),
                                      bytecode->GetBufferSize(), nullptr,
                                      shader.GetAddressOf()),
          operation);
    return shader;
}

float DeviceDepth(float view_depth) {
    return (std::clamp)(kProjectionA - kProjectionB / view_depth, 0.0f,
                       0.999998f);
}

bool IsForegroundSilhouette(UINT x, UINT y) {
    return x >= kForegroundLeft && x < kForegroundRight &&
           y >= kForegroundTop && y < kForegroundBottom;
}

float FixtureViewDepth(UINT x, UINT y, bool hidden_shell) {
    if (!IsForegroundSilhouette(x, y)) {
        return kWallDepth;
    }
    return hidden_shell ? kHiddenFrontDepth : kForegroundDepth;
}

ComPtr<ID3D11Texture2D> CreateDeviceDepthTexture(ID3D11Device* device,
                                                 bool hidden_shell) {
    std::vector<float> values(kWidth * kHeight);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * kWidth + x;
            values[index] = DeviceDepth(FixtureViewDepth(x, y, hidden_shell));
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = values.data();
    initial.SysMemPitch = kWidth * sizeof(float);
    initial.SysMemSlicePitch = kWidth * kHeight * sizeof(float);
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, &initial, texture.GetAddressOf()),
          "CreateTexture2D(device depth)");
    return texture;
}

ComPtr<ID3D11Texture2D> CreateStochasticDepthTexture(ID3D11Device* device,
                                                     bool hidden_shell) {
    constexpr std::uint32_t kFarDeviceDepthBits = 0x3F7FFFEFu;
    std::vector<UInt2> packed_depths(
        static_cast<std::size_t>(kWidth) * kHeight,
        UInt2{kFarDeviceDepthBits, kFarDeviceDepthBits});
    if (hidden_shell) {
        const std::uint32_t shell_depth_bits =
            std::bit_cast<std::uint32_t>(DeviceDepth(kHiddenShellDepth));
        for (UINT y = kForegroundTop; y < kForegroundBottom; ++y) {
            for (UINT x = kForegroundLeft; x < kForegroundRight; ++x) {
                const std::size_t index =
                    static_cast<std::size_t>(y) * kWidth + x;
                packed_depths[index].x = shell_depth_bits;
            }
        }
    }
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = packed_depths.data();
    initial.SysMemPitch = kWidth * sizeof(UInt2);
    initial.SysMemSlicePitch = kWidth * kHeight * sizeof(UInt2);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = kPackedSliceCount;
    desc.Format = DXGI_FORMAT_R32G32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, &initial, texture.GetAddressOf()),
          "CreateTexture2D(stochastic depth)");
    return texture;
}

ComPtr<ID3D11Texture2D> CreateTarget(ID3D11Device* device, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()),
          "CreateTexture2D(target)");
    return texture;
}

ComPtr<ID3D11ShaderResourceView> CreateSrv(ID3D11Device* device,
                                          ID3D11Resource* resource) {
    ComPtr<ID3D11ShaderResourceView> view;
    Check(device->CreateShaderResourceView(resource, nullptr, view.GetAddressOf()),
          "CreateShaderResourceView");
    return view;
}

ComPtr<ID3D11UnorderedAccessView> CreateUav(ID3D11Device* device,
                                           ID3D11Resource* resource) {
    ComPtr<ID3D11UnorderedAccessView> view;
    Check(device->CreateUnorderedAccessView(resource, nullptr, view.GetAddressOf()),
          "CreateUnorderedAccessView");
    return view;
}

template <typename T>
ComPtr<ID3D11Buffer> CreateImmutableConstantBuffer(ID3D11Device* device,
                                                   const T& value) {
    static_assert(sizeof(T) % 16 == 0);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(sizeof(T));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = &value;
    ComPtr<ID3D11Buffer> buffer;
    Check(device->CreateBuffer(&desc, &initial, buffer.GetAddressOf()),
          "CreateBuffer(constants)");
    return buffer;
}

void ClearComputeBindings(ID3D11DeviceContext* context) {
    const std::array<ID3D11ShaderResourceView*, 3> null_srvs{};
    context->CSSetShaderResources(0, static_cast<UINT>(null_srvs.size()),
                                  null_srvs.data());
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
}

void DispatchPass(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
                  const std::array<ID3D11ShaderResourceView*, 3>& resources,
                  UINT resource_count, ID3D11UnorderedAccessView* output) {
    ClearComputeBindings(context);
    context->CSSetShaderResources(0, resource_count, resources.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(shader, nullptr, 0);
    context->Dispatch((kWidth + 7u) / 8u, (kHeight + 7u) / 8u, 1);
}

std::vector<std::uint8_t> ReadTexture(ID3D11Device* device,
                                      ID3D11DeviceContext* context,
                                      ID3D11Texture2D* texture,
                                      UINT bytes_per_pixel) {
    D3D11_TEXTURE2D_DESC source_desc{};
    texture->GetDesc(&source_desc);
    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&staging_desc, nullptr, staging.GetAddressOf()),
          "CreateTexture2D(staging)");
    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "Map(staging)");
    const std::size_t row_size = static_cast<std::size_t>(kWidth) * bytes_per_pixel;
    std::vector<std::uint8_t> result(row_size * kHeight);
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (UINT y = 0; y < kHeight; ++y) {
        std::copy_n(source + static_cast<std::size_t>(y) * mapped.RowPitch,
                    row_size, result.data() + static_cast<std::size_t>(y) * row_size);
    }
    context->Unmap(staging.Get(), 0);
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 7) {
            Fail("Usage: SdaoDetachedHaloValidation <Luma_SD_SDAO.hlsl> "
                 "<raw.bin> <final.bin> <bytecode-directory> "
                 "<sdao|gtao-lite> <detached-front|hidden-shell>");
        }

        const std::filesystem::path shader_path = argv[1];
        const std::filesystem::path raw_path = argv[2];
        const std::filesystem::path final_path = argv[3];
        const std::filesystem::path bytecode_directory = argv[4];
        const std::wstring mode = argv[5];
        const std::wstring fixture = argv[6];
        const bool gtao_lite = mode == L"gtao-lite";
        if (!gtao_lite && mode != L"sdao") {
            Fail("Mode must be either 'sdao' or 'gtao-lite'.");
        }
        const bool hidden_shell = fixture == L"hidden-shell";
        if (!hidden_shell && fixture != L"detached-front") {
            Fail("Fixture must be either 'detached-front' or 'hidden-shell'.");
        }
        const char* lite_value = gtao_lite ? "1" : "0";
        if (!std::filesystem::is_regular_file(shader_path)) {
            Fail("Shader source does not exist: " + shader_path.string());
        }
        std::filesystem::create_directories(bytecode_directory);

        const D3D_SHADER_MACRO main_macros[] = {
            {"SD_SDAO_QUALITY", "2"},
            {"SD_GTAO_LITE", lite_value},
            {nullptr, nullptr},
        };
        const D3D_SHADER_MACRO horizontal_macros[] = {
            {"SD_SDAO_QUALITY", "2"},
            {"SD_SDAO_FILTER_HORIZONTAL", "1"},
            {"SD_SDAO_COLOR_OUTPUT", "0"},
            {nullptr, nullptr},
        };
        const D3D_SHADER_MACRO vertical_macros[] = {
            {"SD_SDAO_QUALITY", "2"},
            {"SD_SDAO_FILTER_HORIZONTAL", "0"},
            {"SD_SDAO_COLOR_OUTPUT", "1"},
            {nullptr, nullptr},
        };

        const auto prepare_bytecode =
            CompileShader(shader_path, "prepare_depth_cs", nullptr);
        const auto main_bytecode =
            CompileShader(shader_path, "main_pass_cs", main_macros);
        const auto horizontal_bytecode =
            CompileShader(shader_path, "spatial_filter_cs", horizontal_macros);
        const auto vertical_bytecode =
            CompileShader(shader_path, "spatial_filter_cs", vertical_macros);
        WriteBytes(bytecode_directory / "prepare_depth_cs.cso",
                   prepare_bytecode->GetBufferPointer(),
                   prepare_bytecode->GetBufferSize());
        WriteBytes(bytecode_directory /
                       (gtao_lite ? "main_pass_cs.q2.gtao-lite.cso"
                                  : "main_pass_cs.q2.sdao.cso"),
                   main_bytecode->GetBufferPointer(), main_bytecode->GetBufferSize());
        WriteBytes(bytecode_directory / "spatial_filter_cs.q2.horizontal.cso",
                   horizontal_bytecode->GetBufferPointer(),
                   horizontal_bytecode->GetBufferSize());
        WriteBytes(bytecode_directory / "spatial_filter_cs.q2.vertical.cso",
                   vertical_bytecode->GetBufferPointer(),
                   vertical_bytecode->GetBufferSize());

        const std::array<D3D_FEATURE_LEVEL, 1> levels{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL feature_level{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(
                  nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                  D3D11_CREATE_DEVICE_SINGLETHREADED, levels.data(),
                  static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                  device.GetAddressOf(), &feature_level, context.GetAddressOf()),
              "D3D11CreateDevice(WARP)");
        if (feature_level != D3D_FEATURE_LEVEL_11_0) {
            Fail("WARP did not expose D3D feature level 11_0.");
        }

        const auto prepare_shader = CreateShader(
            device.Get(), prepare_bytecode.Get(), "CreateComputeShader(prepare)");
        const auto main_shader = CreateShader(
            device.Get(), main_bytecode.Get(), "CreateComputeShader(main)");
        const auto horizontal_shader = CreateShader(
            device.Get(), horizontal_bytecode.Get(), "CreateComputeShader(horizontal)");
        const auto vertical_shader = CreateShader(
            device.Get(), vertical_bytecode.Get(), "CreateComputeShader(vertical)");

        const auto device_depth =
            CreateDeviceDepthTexture(device.Get(), hidden_shell);
        const auto stochastic_depth =
            CreateStochasticDepthTexture(device.Get(), hidden_shell);
        const auto linear_depth = CreateTarget(device.Get(), DXGI_FORMAT_R32_FLOAT);
        const auto raw_ao = CreateTarget(device.Get(), DXGI_FORMAT_R16_FLOAT);
        const auto filtered_ao = CreateTarget(device.Get(), DXGI_FORMAT_R16_FLOAT);
        const auto final_ao = CreateTarget(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

        const auto device_depth_srv = CreateSrv(device.Get(), device_depth.Get());
        const auto stochastic_depth_srv = CreateSrv(device.Get(), stochastic_depth.Get());
        const auto linear_depth_srv = CreateSrv(device.Get(), linear_depth.Get());
        const auto raw_ao_srv = CreateSrv(device.Get(), raw_ao.Get());
        const auto filtered_ao_srv = CreateSrv(device.Get(), filtered_ao.Get());
        const auto linear_depth_uav = CreateUav(device.Get(), linear_depth.Get());
        const auto raw_ao_uav = CreateUav(device.Get(), raw_ao.Get());
        const auto filtered_ao_uav = CreateUav(device.Get(), filtered_ao.Get());
        const auto final_ao_uav = CreateUav(device.Get(), final_ao.Get());

        const std::array<Float4, 4> projection_constants{{
            {static_cast<float>(kWidth), static_cast<float>(kHeight),
             1.0f / static_cast<float>(kWidth), 1.0f / static_cast<float>(kHeight)},
            {kProjectionB, kProjectionB, kProjectionB, kProjectionB},
            {kProjectionA, kProjectionA, kProjectionA, kProjectionA},
            {0.598038971f, 0.336396933f, 0.0f, 0.0f},
        }};
        const SdaoConstants sdao_constants{0.5f, 1.0f, 0.615f, 1.457f};
        const auto projection_buffer =
            CreateImmutableConstantBuffer(device.Get(), projection_constants);
        const auto sdao_buffer =
            CreateImmutableConstantBuffer(device.Get(), sdao_constants);
        ID3D11Buffer* projection = projection_buffer.Get();
        ID3D11Buffer* sdao = sdao_buffer.Get();
        context->CSSetConstantBuffers(9, 1, &projection);
        context->CSSetConstantBuffers(11, 1, &sdao);

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Check(device->CreateSamplerState(&sampler_desc, sampler.GetAddressOf()),
              "CreateSamplerState");
        ID3D11SamplerState* sampler_pointer = sampler.Get();
        context->CSSetSamplers(0, 1, &sampler_pointer);

        DispatchPass(context.Get(), prepare_shader.Get(),
                     {device_depth_srv.Get(), nullptr, nullptr}, 1,
                     linear_depth_uav.Get());
        DispatchPass(context.Get(), main_shader.Get(),
                     {linear_depth_srv.Get(), nullptr,
                      gtao_lite ? nullptr : stochastic_depth_srv.Get()}, 3,
                     raw_ao_uav.Get());
        DispatchPass(context.Get(), horizontal_shader.Get(),
                     {raw_ao_srv.Get(), linear_depth_srv.Get(), nullptr}, 2,
                     filtered_ao_uav.Get());
        DispatchPass(context.Get(), vertical_shader.Get(),
                     {filtered_ao_srv.Get(), linear_depth_srv.Get(), nullptr}, 2,
                     final_ao_uav.Get());
        ClearComputeBindings(context.Get());

        const auto raw_bytes =
            ReadTexture(device.Get(), context.Get(), raw_ao.Get(), 2);
        const auto final_bytes =
            ReadTexture(device.Get(), context.Get(), final_ao.Get(), 4);
        WriteBytes(raw_path, raw_bytes.data(), raw_bytes.size());
        WriteBytes(final_path, final_bytes.data(), final_bytes.size());

        std::cout << "Executed SPatch "
                  << (gtao_lite ? "GTAO-lite" : "SDAO")
                  << " q2 " << (hidden_shell ? "hidden-shell" : "detached-front")
                  << " prepare/main/horizontal/vertical on D3D11 WARP at 64x64 "
                     "(flat wall at view depth 4.4, "
                  << (hidden_shell
                          ? "regular front at 3.0, stochastic shell at 4.2, "
                          : "foreground silhouette at 4.2, ")
                  << (gtao_lite
                          ? "no stochastic SRV).\n"
                          : hidden_shell ? "one controlled hidden layer).\n"
                                         : "stochastic layers far).\n");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
