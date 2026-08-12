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
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Float4 {
    float x;
    float y;
    float z;
    float w;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

Vec3 operator+(Vec3 left, Vec3 right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 operator-(Vec3 left, Vec3 right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 operator*(Vec3 left, Vec3 right) {
    return {left.x * right.x, left.y * right.y, left.z * right.z};
}

Vec3 operator*(Vec3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float Dot(Vec3 left, Vec3 right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

float Saturate(float value) {
    return (std::clamp)(value, 0.0f, 1.0f);
}

Vec3 Saturate(Vec3 value) {
    return {Saturate(value.x), Saturate(value.y), Saturate(value.z)};
}

Vec3 Max(Vec3 value, float floor) {
    return {(std::max)(value.x, floor), (std::max)(value.y, floor),
            (std::max)(value.z, floor)};
}

Vec3 Min(Vec3 value, float ceiling) {
    return {(std::min)(value.x, ceiling), (std::min)(value.y, ceiling),
            (std::min)(value.z, ceiling)};
}

Vec3 SafeNormalize(Vec3 value) {
    const float inverse_length =
        1.0f / std::sqrt((std::max)(Dot(value, value), 1.0e-8f));
    return value * inverse_length;
}

Vec3 Direction(float no_z, float phi) {
    const float transverse = std::sqrt((std::max)(0.0f, 1.0f - no_z * no_z));
    return {transverse * std::cos(phi), transverse * std::sin(phi), no_z};
}

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[192]{};
        sprintf_s(message, "%s failed with HRESULT 0x%08X", operation,
                  static_cast<unsigned int>(result));
        Fail(message);
    }
}

std::vector<std::uint8_t> ReadBytes(const wchar_t* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        Fail("Could not open compiled shader semantic-validation bytecode.");
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0) {
        Fail("Compiled shader semantic-validation bytecode is empty.");
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        Fail("Could not read compiled shader semantic-validation bytecode.");
    }
    return bytes;
}

ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* device, UINT size) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    Check(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()),
          "CreateBuffer(constants)");
    return buffer;
}

ComPtr<ID3D11Texture2D> CreateTexture(
    ID3D11Device* device, UINT bind_flags, D3D11_USAGE usage,
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
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()),
          "CreateTexture2D");
    return texture;
}

struct Binding {
    UINT slot;
    ID3D11Buffer* buffer;
};

Float4 RunPixelShader(
    ID3D11DeviceContext* context, ID3D11VertexShader* vertex_shader,
    ID3D11PixelShader* pixel_shader, ID3D11RenderTargetView* output_rtv,
    ID3D11Texture2D* output, ID3D11Texture2D* staging,
    const std::vector<Binding>& bindings) {
    context->ClearState();
    constexpr float kClear[4] = {-777.0f, -777.0f, -777.0f, -777.0f};
    context->ClearRenderTargetView(output_rtv, kClear);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    for (const Binding& binding : bindings) {
        ID3D11Buffer* buffer = binding.buffer;
        context->PSSetConstantBuffers(binding.slot, 1, &buffer);
    }
    ID3D11RenderTargetView* target = output_rtv;
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(staging, output);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped),
          "Map(semantic output)");
    const Float4 result = *static_cast<const Float4*>(mapped.pData);
    context->Unmap(staging, 0);
    return result;
}

Vec3 BoundF0(Vec3 f0) {
    return Min(Max(f0, 0.0f), 0.999f);
}

Vec3 NativeOpaqueF90(Vec3 f0) {
    return Min(BoundF0(f0) * Vec3{50.0f, 50.0f, 50.0f}, 1.0f);
}

Vec3 Fresnel(float cosine, Vec3 f0, Vec3 f90) {
    const Vec3 bounded_f0 = BoundF0(f0);
    const Vec3 bounded_f90 = Min(
        {(std::max)(f90.x, bounded_f0.x),
         (std::max)(f90.y, bounded_f0.y),
         (std::max)(f90.z, bounded_f0.z)},
        1.0f);
    const float one_minus = 1.0f - Saturate(cosine);
    const float one_minus2 = one_minus * one_minus;
    const float one_minus5 = one_minus2 * one_minus2 * one_minus;
    return bounded_f0 + (bounded_f90 - bounded_f0) * one_minus5;
}

Vec3 DirectSpecular(
    Vec3 normal, Vec3 view, Vec3 light, float native_exponent, Vec3 f0,
    float geometric_variance, Vec3 f90) {
    const float no_v = Saturate(Dot(normal, view));
    const float no_l = Saturate(Dot(normal, light));
    const Vec3 half_vector = SafeNormalize(view + light);
    const float no_h = Saturate(Dot(normal, half_vector));
    const float safe_exponent = (std::clamp)(native_exponent, 0.0f, 4096.0f);
    const float alpha_squared = Saturate(
        2.0f / (safe_exponent + 2.0f) +
        (std::max)(geometric_variance, 0.0f));
    const float denominator = (std::max)(
        no_h * no_h * (alpha_squared - 1.0f) + 1.0f, 1.0e-4f);
    const float distribution =
        alpha_squared / (denominator * denominator);
    const float smith_v = no_l * std::sqrt(
        no_v * no_v * (1.0f - alpha_squared) + alpha_squared);
    const float smith_l = no_v * std::sqrt(
        no_l * no_l * (1.0f - alpha_squared) + alpha_squared);
    const float visibility =
        0.5f / (std::max)(smith_v + smith_l, 1.0e-6f);
    return Fresnel(Dot(view, half_vector), f0, f90) *
           (distribution * visibility);
}

Vec3 GlassDirectSpecular(
    Vec3 normal, Vec3 view, Vec3 light, float native_exponent, Vec3 f0) {
    const float no_v = Saturate(Dot(normal, view));
    const float no_l = Saturate(Dot(normal, light));
    const Vec3 half_vector = SafeNormalize(view + light);
    const float no_h = Saturate(Dot(normal, half_vector));
    const float safe_exponent = (std::clamp)(native_exponent, 0.0f, 65536.0f);
    const float alpha_squared = Saturate(2.0f / (safe_exponent + 2.0f));
    const float denominator = (std::max)(
        no_h * no_h * (alpha_squared - 1.0f) + 1.0f, 1.0e-6f);
    const float distribution =
        alpha_squared / (denominator * denominator);
    const float smith_v = no_l * std::sqrt(
        no_v * no_v * (1.0f - alpha_squared) + alpha_squared);
    const float smith_l = no_v * std::sqrt(
        no_l * no_l * (1.0f - alpha_squared) + alpha_squared);
    const float visibility =
        0.5f / (std::max)(smith_v + smith_l, 1.0e-6f);
    return Fresnel(Dot(view, half_vector), f0, {1.0f, 1.0f, 1.0f}) *
           (distribution * visibility);
}

Vec3 DiffuseWeight(
    Vec3 normal, Vec3 view, Vec3 light, Vec3 f0, float metallic, Vec3 f90) {
    const Vec3 one{1.0f, 1.0f, 1.0f};
    const Vec3 view_transmission = one - Fresnel(Dot(normal, view), f0, f90);
    const Vec3 light_transmission = one - Fresnel(Dot(normal, light), f0, f90);
    return view_transmission * light_transmission *
           (1.0f - Saturate(metallic));
}

struct PbrCase {
    const char* name;
    std::uint32_t operation;
    Vec3 normal;
    Vec3 view;
    Vec3 light;
    Vec3 f0;
    Vec3 f90;
    float cosine;
    float exponent;
    float variance;
    float metallic;
};

Vec3 Expected(const PbrCase& test) {
    switch (test.operation) {
        case 0:
            return Fresnel(test.cosine, test.f0, test.f90);
        case 1:
            return DirectSpecular(
                test.normal, test.view, test.light, test.exponent, test.f0,
                test.variance, test.f90);
        case 2:
            return DiffuseWeight(
                test.normal, test.view, test.light, test.f0, test.metallic,
                test.f90);
        case 3:
            return GlassDirectSpecular(
                test.normal, test.view, test.light, test.exponent, test.f0);
        case 4:
            return NativeOpaqueF90(test.f0);
        default:
            Fail("Unknown PBR semantic operation.");
    }
}

std::array<Float4, 6> Pack(const PbrCase& test) {
    return {{
        {test.operation == 0 ? test.cosine : test.normal.x,
         test.normal.y, test.normal.z, 0.0f},
        {test.view.x, test.view.y, test.view.z, 0.0f},
        {test.light.x, test.light.y, test.light.z, 0.0f},
        {test.f0.x, test.f0.y, test.f0.z, 0.0f},
        {test.f90.x, test.f90.y, test.f90.z, 0.0f},
        {static_cast<float>(test.operation), test.exponent, test.variance,
         test.metallic},
    }};
}

Vec3 WaterScattering(
    float transmittance, Vec3 scene_albedo, Vec3 direct_lighting,
    Vec3 absorb_extinct, float strength) {
    strength = (std::clamp)(strength, 0.0f, 2.0f);
    if (strength <= 0.0f) {
        return {};
    }
    const Vec3 bounded_direct = Min(Max(direct_lighting, 0.0f), 65504.0f);
    const Vec3 bounded_albedo = Max(Saturate(scene_albedo), 0.04f);
    const Vec3 recovered = Min(
        {bounded_direct.x / bounded_albedo.x,
         bounded_direct.y / bounded_albedo.y,
         bounded_direct.z / bounded_albedo.z},
        65504.0f);
    const float scatter_weight = strength * 0.0795774683f *
                                 (1.0f - Saturate(transmittance));
    const Vec3 medium_albedo = Min(Max(
        absorb_extinct * absorb_extinct, 0.0f), 1.0f);
    return recovered * medium_albedo * scatter_weight;
}

struct WaterCase {
    const char* name;
    float transmittance;
    Vec3 absorb_extinct;
    Vec3 scene_albedo;
    Vec3 direct_lighting;
};

Float4 Expected(const WaterCase& test) {
    const Vec3 scattering = WaterScattering(
        test.transmittance, test.scene_albedo, test.direct_lighting,
        test.absorb_extinct, 1.0f);
    return {scattering.x, scattering.y, scattering.z, 1.0f};
}

void ValidateValue(
    const char* family, const char* name, std::size_t channel,
    float expected, float actual, float& maximum_error,
    std::size_t& comparisons) {
    const float error = std::abs(actual - expected);
    maximum_error = (std::max)(maximum_error, error);
    const float tolerance = 2.0e-5f + 2.0e-4f * std::abs(expected);
    if (!std::isfinite(actual) || error > tolerance) {
        std::cerr << family << " compiled-HLSL mismatch for " << name
                  << " channel " << channel << ": expected " << expected
                  << ", got " << actual << " (tolerance " << tolerance
                  << ")\n";
        throw std::runtime_error("Compiled shader semantic validation failed.");
    }
    ++comparisons;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) {
            Fail("Usage: SPatchShaderSemanticValidation <pbr.cso> <water.cso>");
        }

        UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
        const std::array<D3D_FEATURE_LEVEL, 2> levels{
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL feature_level{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(
                  nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels.data(),
                  static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                  device.GetAddressOf(), &feature_level, context.GetAddressOf()),
              "D3D11CreateDevice(WARP)");

        static constexpr char kVertexShader[] = R"(
struct Output { float4 position : SV_Position; };
Output main(uint id : SV_VertexID) {
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
})";
        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> errors;
        Check(D3DCompile(
                  kVertexShader, sizeof(kVertexShader) - 1, "SemanticValidationVS",
                  nullptr, nullptr, "main", "vs_4_0",
                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS |
                      D3DCOMPILE_OPTIMIZATION_LEVEL3,
                  0, vertex_bytecode.GetAddressOf(), errors.GetAddressOf()),
              "D3DCompile(validation VS)");
        ComPtr<ID3D11VertexShader> vertex_shader;
        Check(device->CreateVertexShader(
                  vertex_bytecode->GetBufferPointer(),
                  vertex_bytecode->GetBufferSize(), nullptr,
                  vertex_shader.GetAddressOf()),
              "CreateVertexShader");

        const auto pbr_bytecode = ReadBytes(argv[1]);
        const auto water_bytecode = ReadBytes(argv[2]);
        ComPtr<ID3D11PixelShader> pbr_shader;
        ComPtr<ID3D11PixelShader> water_shader;
        Check(device->CreatePixelShader(
                  pbr_bytecode.data(), pbr_bytecode.size(), nullptr,
                  pbr_shader.GetAddressOf()),
              "CreatePixelShader(PBR semantic entry)");
        Check(device->CreatePixelShader(
                  water_bytecode.data(), water_bytecode.size(), nullptr,
                  water_shader.GetAddressOf()),
              "CreatePixelShader(water semantic entry)");

        const auto output = CreateTexture(
            device.Get(), D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT);
        const auto staging = CreateTexture(
            device.Get(), 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        ComPtr<ID3D11RenderTargetView> output_rtv;
        Check(device->CreateRenderTargetView(
                  output.Get(), nullptr, output_rtv.GetAddressOf()),
              "CreateRenderTargetView");

        const auto pbr_constants = CreateConstantBuffer(
            device.Get(), sizeof(Float4) * 6);
        const auto water_inputs = CreateConstantBuffer(
            device.Get(), sizeof(Float4) * 4);
        const auto water_look = CreateConstantBuffer(
            device.Get(), sizeof(Float4) * 4);
        const auto water_spatch = CreateConstantBuffer(
            device.Get(), sizeof(Float4));

        const Vec3 normal{0.0f, 0.0f, 1.0f};
        const std::array<PbrCase, 15> pbr_cases{{
            {"fresnel-authored-f90", 0, normal, {}, {}, {0.04f, 0.08f, 0.2f},
             {1.0f, 0.8f, 0.5f}, 0.8f},
            {"fresnel-grazing", 0, normal, {}, {}, {0.05f, 0.05f, 0.05f},
             {1.0f, 1.0f, 1.0f}, 0.02f},
            {"fresnel-input-clamps", 0, normal, {}, {}, {-1.0f, 0.5f, 4.0f},
             {0.2f, 0.1f, 2.0f}, -0.5f},
            {"direct-rough", 1, normal, Direction(0.8f, 0.0f),
             Direction(0.6f, 1.1f), {0.04f, 0.08f, 0.2f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f},
            {"direct-mid-variance", 1, normal, Direction(0.5f, 0.3f),
             Direction(0.7f, 2.0f), {0.02f, 0.3f, 0.8f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 32.0f, 0.02f},
            {"direct-smooth", 1, normal, Direction(0.98f, 0.1f),
             Direction(0.97f, 0.12f), {0.04f, 0.04f, 0.04f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 1024.0f, 0.0f},
            {"direct-upper-clamp", 1, normal, Direction(0.9f, 0.2f),
             Direction(0.85f, 0.3f), {0.1f, 0.2f, 0.3f},
             {0.6f, 0.7f, 0.8f}, 0.0f, 9000.0f, -0.5f},
            {"direct-lower-clamp", 1, normal, Direction(0.7f, 0.7f),
             Direction(0.4f, 2.4f), {0.0f, 0.5f, 1.5f},
             {0.0f, 0.9f, 1.5f}, 0.0f, -20.0f, 0.0f},
            {"diffuse-dielectric", 2, normal, Direction(0.8f, 0.0f),
             Direction(0.6f, 1.0f), {0.04f, 0.08f, 0.2f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f, 0.0f},
            {"diffuse-metal", 2, normal, Direction(0.4f, 0.0f),
             Direction(0.7f, 1.4f), {0.8f, 0.6f, 0.2f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f, 1.0f},
            {"diffuse-partial", 2, normal, Direction(0.2f, 0.0f),
             Direction(0.9f, 2.2f), {-0.2f, 0.3f, 2.0f},
             {0.1f, 0.7f, 1.4f}, 0.0f, 0.0f, 0.0f, 0.4f},
            {"glass-mid", 3, normal, Direction(0.8f, 0.2f),
             Direction(0.75f, 0.4f), {0.05f, 0.05f, 0.05f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 64.0f},
            {"glass-smooth", 3, normal, Direction(0.99f, 0.1f),
             Direction(0.985f, 0.11f), {0.05f, 0.1f, 0.2f},
             {1.0f, 1.0f, 1.0f}, 0.0f, 32767.0f},
            {"native-f90", 4, normal, {}, {}, {0.0f, 0.01f, 0.2f},
             {}, 0.0f},
            {"native-f90-clamps", 4, normal, {}, {}, {-2.0f, 0.5f, 4.0f},
             {}, 0.0f},
        }};

        std::size_t comparisons = 0;
        float maximum_error = 0.0f;
        for (const PbrCase& test : pbr_cases) {
            const auto constants = Pack(test);
            context->UpdateSubresource(
                pbr_constants.Get(), 0, nullptr, constants.data(), 0, 0);
            const Float4 actual = RunPixelShader(
                context.Get(), vertex_shader.Get(), pbr_shader.Get(),
                output_rtv.Get(), output.Get(), staging.Get(),
                {{12, pbr_constants.Get()}});
            const Vec3 expected = Expected(test);
            const std::array<float, 4> observed{
                actual.x, actual.y, actual.z, actual.w};
            const std::array<float, 4> reference{
                expected.x, expected.y, expected.z, 1.0f};
            for (std::size_t channel = 0; channel < observed.size(); ++channel) {
                ValidateValue("PBR", test.name, channel, reference[channel],
                              observed[channel], maximum_error, comparisons);
            }
        }

        const std::array<WaterCase, 4> water_cases{{
            {"scattering-fixed-strength", 0.25f,
             {0.1f, 0.5f, 2.0f},
             {0.2f, 0.5f, 1.0f}, {0.1f, 0.4f, 2.0f}},
            {"scattering-input-clamps", -1.0f,
             {-2.0f, 0.5f, 4.0f},
             {-1.0f, 0.0f, 2.0f}, {-3.0f, 70000.0f, 8.0f}},
            {"scattering-fully-transmitted", 1.0f,
             {0.5f, 0.5f, 0.5f},
             {0.2f, 0.4f, 0.8f}, {1.0f, 2.0f, 3.0f}},
            {"scattering-zero-direct-light", 0.0f,
             {1.0f, 1.0f, 1.0f},
             {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.0f}},
        }};

        for (const WaterCase& test : water_cases) {
            const std::array<Float4, 4> inputs{{
                {0.0f, test.transmittance, 0.0f, 0.0f},
                {0.0f, 0.0f, 1.0f, 1.0f},
                {test.scene_albedo.x, test.scene_albedo.y, test.scene_albedo.z, 0.0f},
                {test.direct_lighting.x, test.direct_lighting.y,
                 test.direct_lighting.z, 0.0f},
            }};
            const std::array<Float4, 4> look{{
                {test.absorb_extinct.x, test.absorb_extinct.y,
                 test.absorb_extinct.z, 0.0f},
                {},
                {},
                {},
            }};
            const Float4 spatch{1.0f, 0.0f, 0.0f, 0.0f};
            context->UpdateSubresource(
                water_inputs.Get(), 0, nullptr, inputs.data(), 0, 0);
            context->UpdateSubresource(
                water_look.Get(), 0, nullptr, look.data(), 0, 0);
            context->UpdateSubresource(
                water_spatch.Get(), 0, nullptr, &spatch, 0, 0);
            const Float4 actual = RunPixelShader(
                context.Get(), vertex_shader.Get(), water_shader.Get(),
                output_rtv.Get(), output.Get(), staging.Get(),
                {{4, water_look.Get()}, {12, water_inputs.Get()},
                 {13, water_spatch.Get()}});
            const Float4 expected = Expected(test);
            const std::array<float, 4> observed{
                actual.x, actual.y, actual.z, actual.w};
            const std::array<float, 4> reference{
                expected.x, expected.y, expected.z, expected.w};
            for (std::size_t channel = 0; channel < observed.size(); ++channel) {
                ValidateValue("Water", test.name, channel, reference[channel],
                              observed[channel], maximum_error, comparisons);
            }
        }

        std::cout
            << "Validated production PBR and Water HLSL helpers on D3D11 WARP "
            << "across " << pbr_cases.size() << " PBR and "
            << water_cases.size() << " Water vectors (" << comparisons
            << " channel comparisons, maximum absolute error "
            << maximum_error << ").\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
