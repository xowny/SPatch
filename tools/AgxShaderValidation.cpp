#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../luma/validation/AgxToneMapping.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Float4 {
    float x;
    float y;
    float z;
    float w;
};

struct Variant {
    const wchar_t* name;
    spatch::agx::Look look;
    float strength;
    bool full_strength;
};

struct Vector {
    const char* name;
    spatch::agx::Rgb scene;
    Float4 bloom;
    float white_scale;
    float gamma;
    bool raw_encoded_scene = false;
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[160]{};
        sprintf_s(message, "%s failed with HRESULT 0x%08X", operation,
                  static_cast<unsigned int>(result));
        Fail(message);
    }
}

std::vector<std::uint8_t> ReadBytes(const wchar_t* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        Fail("Could not open compiled AgX shader bytecode.");
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0) {
        Fail("Compiled AgX shader bytecode is empty.");
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        Fail("Could not read compiled AgX shader bytecode.");
    }
    return bytes;
}

float EncodeScene(float linear) {
    linear = (std::max)(linear, 0.0f);
    return 1.04f * linear / (linear + 0.2f);
}

float Saturate(float value) {
    return (std::max)(0.0f, (std::min)(1.0f, value));
}

float FiniteOrZero(float value) {
    return std::isfinite(value) && std::abs(value) <= 65504.0f ? value : 0.0f;
}

float DecodeScene(float value) {
    constexpr float kMaximumEncoding = 1.04f - 1.0e-5f;
    value = (std::max)(0.0f, (std::min)(kMaximumEncoding, FiniteOrZero(value)));
    return (std::min)(65504.0f, 0.2f * value /
        (std::max)(1.04f - value, 1.0e-5f));
}

spatch::agx::Rgb Expected(const Vector& input, const Variant& variant) {
    const auto encoded = [&](float scene) {
        return input.raw_encoded_scene ? scene : EncodeScene(scene);
    };
    const spatch::agx::Rgb decoded{
        DecodeScene(encoded(input.scene.r)),
        DecodeScene(encoded(input.scene.g)),
        DecodeScene(encoded(input.scene.b))};
    const Float4 safe_bloom{
        FiniteOrZero(input.bloom.x), FiniteOrZero(input.bloom.y),
        FiniteOrZero(input.bloom.z), FiniteOrZero(input.bloom.w)};
    const auto expose = [&](float scene) {
        return (std::min)(65504.0f, (std::max)(
            0.0f, scene * (1.0f + safe_bloom.w * (scene - 1.0f))));
    };
    const spatch::agx::Rgb exposed{
        expose(decoded.r), expose(decoded.g), expose(decoded.b)};
    const spatch::agx::Rgb agx = spatch::agx::EvaluateGameMappedRgb(
        exposed, input.white_scale, 1.0f, variant.look);
    const spatch::agx::Rgb stock{
        spatch::agx::EvaluateStockMapped(exposed.r, input.white_scale),
        spatch::agx::EvaluateStockMapped(exposed.g, input.white_scale),
        spatch::agx::EvaluateStockMapped(exposed.b, input.white_scale)};
    const auto blend = [&](float native_value, float agx_value) {
        return variant.full_strength
            ? agx_value
            : native_value + variant.strength * (agx_value - native_value);
    };
    spatch::agx::Rgb mapped{
        blend(stock.r, agx.r), blend(stock.g, agx.g), blend(stock.b, agx.b)};
    const auto compose = [&](float mapped_value, float bloom_value) {
        const float with_bloom =
            1.0f - (1.0f - bloom_value) * (1.0f - mapped_value);
        return std::pow((std::max)(with_bloom, 0.0f), input.gamma);
    };
    spatch::agx::Rgb composed{
        compose(mapped.r, safe_bloom.x),
        compose(mapped.g, safe_bloom.y),
        compose(mapped.b, safe_bloom.z)};
    const spatch::agx::Rgb limited = spatch::agx::LimitDisplayPeak(composed);
    if (variant.full_strength) {
        return limited;
    }
    return spatch::agx::Rgb{
        composed.r + variant.strength * (limited.r - composed.r),
        composed.g + variant.strength * (limited.g - composed.g),
        composed.b + variant.strength * (limited.b - composed.b)};
}

ComPtr<ID3D11Texture2D> CreateTexture(ID3D11Device* device,
                                     const Float4& value,
                                     UINT bind_flags,
                                     D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                                     UINT cpu_access = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = cpu_access;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = &value;
    initial.SysMemPitch = sizeof(Float4);
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc,
                                  usage == D3D11_USAGE_DEFAULT ? &initial : nullptr,
                                  texture.GetAddressOf()),
          "CreateTexture2D");
    return texture;
}

Float4 RunVector(ID3D11Device* device,
                 ID3D11DeviceContext* context,
                 ID3D11VertexShader* vertex_shader,
                 ID3D11PixelShader* pixel_shader,
                 ID3D11SamplerState* sampler,
                 ID3D11Buffer* constants,
                 const Vector& input) {
    const Float4 encoded{
        input.raw_encoded_scene ? input.scene.r : EncodeScene(input.scene.r),
        input.raw_encoded_scene ? input.scene.g : EncodeScene(input.scene.g),
        input.raw_encoded_scene ? input.scene.b : EncodeScene(input.scene.b),
        1.0f};
    const auto scene = CreateTexture(device, encoded, D3D11_BIND_SHADER_RESOURCE);
    const auto bloom = CreateTexture(device, input.bloom, D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11ShaderResourceView> scene_srv;
    ComPtr<ID3D11ShaderResourceView> bloom_srv;
    Check(device->CreateShaderResourceView(scene.Get(), nullptr, scene_srv.GetAddressOf()),
          "CreateShaderResourceView(scene)");
    Check(device->CreateShaderResourceView(bloom.Get(), nullptr, bloom_srv.GetAddressOf()),
          "CreateShaderResourceView(bloom)");

    const Float4 zero{};
    const auto output = CreateTexture(device, zero, D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> output_rtv;
    Check(device->CreateRenderTargetView(output.Get(), nullptr, output_rtv.GetAddressOf()),
          "CreateRenderTargetView");
    const auto staging = CreateTexture(
        device, zero, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);

    std::array<Float4, 8> constant_data{};
    constant_data[0].y = input.white_scale;
    constant_data[0].w = input.gamma;
    context->UpdateSubresource(constants, 0, nullptr, constant_data.data(), 0, 0);

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    ID3D11Buffer* constant_array[] = {constants};
    context->PSSetConstantBuffers(0, 1, constant_array);
    ID3D11SamplerState* samplers[] = {sampler, sampler};
    context->PSSetSamplers(0, 2, samplers);
    ID3D11ShaderResourceView* resources[] = {scene_srv.Get(), bloom_srv.Get()};
    context->PSSetShaderResources(0, 2, resources);
    ID3D11RenderTargetView* target = output_rtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* null_resources[] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, null_resources);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(output)");
    const Float4 result = *static_cast<const Float4*>(mapped.pData);
    context->Unmap(staging.Get(), 0);
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 5) {
            Fail("Usage: AgxShaderValidation <medium-high> <medium-high-blended> <neutral> <neutral-blended>");
        }

        UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const std::array<D3D_FEATURE_LEVEL, 2> levels{
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL feature_level{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                levels.data(), static_cast<UINT>(levels.size()),
                                D3D11_SDK_VERSION, device.GetAddressOf(),
                                &feature_level, context.GetAddressOf()),
              "D3D11CreateDevice(WARP)");

        static constexpr char kVertexShader[] = R"(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
})";
        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> errors;
        Check(D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "AgxValidationVS",
                         nullptr, nullptr, "main", "vs_4_0",
                         D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS |
                             D3DCOMPILE_OPTIMIZATION_LEVEL3,
                         0, vertex_bytecode.GetAddressOf(), errors.GetAddressOf()),
              "D3DCompile(validation VS)");
        ComPtr<ID3D11VertexShader> vertex_shader;
        Check(device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                         vertex_bytecode->GetBufferSize(), nullptr,
                                         vertex_shader.GetAddressOf()),
              "CreateVertexShader");

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Check(device->CreateSamplerState(&sampler_desc, sampler.GetAddressOf()),
              "CreateSamplerState");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = sizeof(Float4) * 8;
        constant_desc.Usage = D3D11_USAGE_DEFAULT;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> constants;
        Check(device->CreateBuffer(&constant_desc, nullptr, constants.GetAddressOf()),
              "CreateBuffer(constants)");

        const std::array<Variant, 4> variants{{
            {argv[1], spatch::agx::Look::MediumHigh, 1.0f, true},
            {argv[2], spatch::agx::Look::MediumHigh, 0.5f, false},
            {argv[3], spatch::agx::Look::Neutral, 1.0f, true},
            {argv[4], spatch::agx::Look::Neutral, 0.5f, false},
        }};
        const float nan = (std::numeric_limits<float>::quiet_NaN)();
        const float infinity = (std::numeric_limits<float>::infinity)();
        const std::array<Vector, 7> vectors{{
            {"neutral", {0.18f, 0.18f, 0.18f}, {0, 0, 0, 0}, 1.0f, 1.0f},
            {"warm", {3.0f, 0.8f, 0.2f}, {0.03f, 0.01f, 0.0f, 0.04f}, 1.0f, 1.0f},
            {"cool", {0.2f, 0.8f, 3.0f}, {0.0f, 0.01f, 0.03f, 0.02f}, 1.0f, 1.0f},
            {"shadow", {0.001f, 0.002f, 0.004f}, {0, 0, 0, 0}, 1.0f, 1.0f},
            {"highlight", {500.0f, 20.0f, 2.0f}, {0, 0, 0, 0}, 1.0f, 1.0f},
            {"authored-controls", {1.5f, 0.4f, 0.1f}, {0.02f, 0.01f, 0.0f, 0.03f}, 1.2f, 1.1f},
            {"non-finite-guards", {nan, infinity, -infinity},
             {nan, infinity, -infinity, infinity}, 1.0f, 1.0f, true},
        }};

        // WARP and the scalar CPU reference differ only by normal shader
        // rounding (currently about 1.5e-5). Keep enough headroom for SDK/OS
        // compiler drift while still catching matrix, polynomial, or domain
        // mistakes that the reflection-only gate cannot see.
        constexpr float kTolerance = 5.0e-4f;
        std::size_t comparisons = 0;
        float maximum_error = 0.0f;
        for (const Variant& variant : variants) {
            const auto bytecode = ReadBytes(variant.name);
            ComPtr<ID3D11PixelShader> pixel_shader;
            Check(device->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr,
                                            pixel_shader.GetAddressOf()),
                  "CreatePixelShader");
            for (const Vector& vector : vectors) {
                const Float4 actual = RunVector(
                    device.Get(), context.Get(), vertex_shader.Get(), pixel_shader.Get(),
                    sampler.Get(), constants.Get(), vector);
                const auto expected = Expected(vector, variant);
                const std::array<float, 3> observed{actual.x, actual.y, actual.z};
                const std::array<float, 3> reference{expected.r, expected.g, expected.b};
                for (std::size_t channel = 0; channel < observed.size(); ++channel) {
                    const float error = std::abs(observed[channel] - reference[channel]);
                    maximum_error = (std::max)(maximum_error, error);
                    if (!std::isfinite(observed[channel]) || error > kTolerance) {
                        std::cerr << "AgX GPU/CPU mismatch for " << vector.name
                                  << " channel " << channel << ": expected "
                                  << reference[channel] << ", got " << observed[channel]
                                  << "\n";
                        return EXIT_FAILURE;
                    }
                    ++comparisons;
                }
                if (actual.w != 1.0f) {
                    std::cerr << "AgX shader returned a non-opaque alpha for "
                              << vector.name << ": " << actual.w << "\n";
                    return EXIT_FAILURE;
                }
            }
        }
        std::cout << "Validated 4 AgX shader variants on D3D11 WARP across 7 RGB vectors ("
                  << comparisons << " channel comparisons, maximum absolute error "
                  << maximum_error << ").\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
