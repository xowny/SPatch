#include "SmaaRuntime.h"

#include "Logger.h"
#include "HookTargetGuard.h"
#include "SystemLibrary.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"
#include "SmaaShaderSource.inl"

namespace spatch::smaa {
namespace {

using Microsoft::WRL::ComPtr;
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID,
                                      SIZE_T,
                                      LPCSTR,
                                      const D3D_SHADER_MACRO*,
                                      ID3DInclude*,
                                      LPCSTR,
                                      LPCSTR,
                                      UINT,
                                      UINT,
                                      ID3DBlob**,
                                      ID3DBlob**);

constexpr UINT kPresentVtableIndex = 8;
constexpr UINT kResizeBuffersVtableIndex = 13;
constexpr std::size_t kOwnedHookPatchBytes = 16;
static_assert(kOwnedHookPatchBytes % sizeof(std::uint64_t) == 0);

struct AtomicHookByteSnapshot {
    static constexpr std::size_t kWordCount = kOwnedHookPatchBytes / sizeof(std::uint64_t);
    std::array<std::atomic<std::uint64_t>, kWordCount> words{};

    void Clear() noexcept {
        for (auto& word : words) {
            word.store(0, std::memory_order_relaxed);
        }
    }

    void Store(const std::array<std::uint8_t, kOwnedHookPatchBytes>& bytes) noexcept {
        for (std::size_t index = 0; index < words.size(); ++index) {
            std::uint64_t value = 0;
            std::memcpy(&value,
                        bytes.data() + index * sizeof(value),
                        sizeof(value));
            words[index].store(value, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool Matches(
        const std::array<std::uint8_t, kOwnedHookPatchBytes>& bytes) const noexcept {
        for (std::size_t index = 0; index < words.size(); ++index) {
            std::uint64_t value = 0;
            std::memcpy(&value,
                        bytes.data() + index * sizeof(value),
                        sizeof(value));
            if (words[index].load(std::memory_order_relaxed) != value) {
                return false;
            }
        }
        return true;
    }
};

struct RedirectChainCapture {
    void* vtable_target = nullptr;
    void* relay = nullptr;
    void* hook_target = nullptr;
    std::array<std::uint8_t, kOwnedHookPatchBytes> vtable_bytes{};
    std::array<std::uint8_t, kOwnedHookPatchBytes> relay_bytes{};
    bool steam_overlay = false;
};

struct AtomicRedirectChainSnapshot {
    std::atomic<void*> vtable_target{nullptr};
    std::atomic<void*> relay{nullptr};
    std::atomic<void*> hook_target{nullptr};
    AtomicHookByteSnapshot vtable_bytes{};
    AtomicHookByteSnapshot relay_bytes{};
    std::atomic<bool> active{false};

    void Clear() noexcept {
        active.store(false, std::memory_order_release);
        vtable_target.store(nullptr, std::memory_order_relaxed);
        relay.store(nullptr, std::memory_order_relaxed);
        hook_target.store(nullptr, std::memory_order_relaxed);
        vtable_bytes.Clear();
        relay_bytes.Clear();
    }

    void Store(const RedirectChainCapture& capture) noexcept {
        active.store(false, std::memory_order_release);
        vtable_target.store(capture.vtable_target, std::memory_order_relaxed);
        relay.store(capture.relay, std::memory_order_relaxed);
        hook_target.store(capture.hook_target, std::memory_order_relaxed);
        vtable_bytes.Store(capture.vtable_bytes);
        relay_bytes.Store(capture.relay_bytes);
        active.store(capture.steam_overlay, std::memory_order_release);
    }
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags);
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* swapchain,
                                              UINT buffer_count,
                                              UINT width,
                                              UINT height,
                                              DXGI_FORMAT new_format,
                                              UINT swap_chain_flags);

PresentFn g_present_original = nullptr;
ResizeBuffersFn g_resize_buffers_original = nullptr;
std::atomic<void*> g_present_hook_target{nullptr};
std::atomic<void*> g_resize_buffers_hook_target{nullptr};
std::atomic<void*> g_present_vtable_target{nullptr};
std::atomic<void*> g_resize_vtable_target{nullptr};
AtomicRedirectChainSnapshot g_present_redirect_chain{};
AtomicRedirectChainSnapshot g_resize_redirect_chain{};
AtomicHookByteSnapshot g_present_hook_bytes{};
AtomicHookByteSnapshot g_resize_buffers_hook_bytes{};
std::atomic<bool> g_present_hook_bytes_captured{false};
std::atomic<bool> g_resize_hook_bytes_captured{false};
std::atomic<bool> g_owned_hook_bytes_valid{false};
// A permanently modified DXGI target (for example an overlay trampoline) is
// not safe to chain. Remember the rejected pair and the swap-chain instance
// so a failed validation does not emit a log line and repeat PE mapping every
// frame. ResizeBuffers clears the cache, and the bounded retry interval below
// lets an overlay replace its trampoline without requiring a process restart.
std::atomic<void*> g_rejected_present_target{nullptr};
std::atomic<void*> g_rejected_resize_target{nullptr};
std::atomic<void*> g_rejected_swapchain{nullptr};
std::atomic<unsigned long long> g_rejected_retry_after_tick{0};
std::atomic<bool> g_rejected_log_emitted{false};

std::uintptr_t g_device_slot = 0;
std::uintptr_t g_context_slot = 0;
std::uintptr_t g_swapchain_slot = 0;

std::atomic<bool> g_enabled = true;
std::atomic<bool> g_debug_keys_enabled = true;
std::atomic<bool> g_any_hook_retained = false;
std::atomic<unsigned long long> g_present_count = 0;
std::atomic<unsigned long long> g_apply_count = 0;
std::atomic<unsigned long long> g_fail_count = 0;
std::atomic<unsigned long long> g_resize_count = 0;
std::atomic<unsigned int> g_width = 0;
std::atomic<unsigned int> g_height = 0;
std::atomic<bool> g_resources_ready = false;
std::atomic<bool> g_sized_resources_ready = false;
std::atomic<bool> g_successful_pass = false;
std::atomic<IDXGISwapChain*> g_successful_swapchain = nullptr;
std::atomic<ID3D11Device*> g_successful_device = nullptr;
std::atomic<unsigned long long> g_resource_generation = 1;
std::atomic<unsigned long long> g_successful_generation = 0;
std::atomic<int> g_preset = 3;
std::atomic<unsigned long long> g_static_retry_after_tick = 0;
std::atomic<unsigned long long> g_sized_retry_after_tick = 0;
std::atomic<unsigned long long> g_hook_retry_after_tick = 0;
// Discovery is triggered at startup and after ResizeBuffers. Once a complete
// pair is owned, periodic validation is enough to notice an overlay/vtable
// replacement; probing every render detour would add avoidable COM/vtable
// traffic at high FPS.
std::atomic<bool> g_hook_discovery_needed = true;
std::atomic<unsigned long long> g_hook_next_probe_tick = 0;
// Parent game detours remain callable until after smaa::Shutdown returns. Gate
// discovery separately so none of those callers can reinstall a DXGI hook in
// the gap between SMAA teardown and process-wide MinHook teardown.
std::atomic<bool> g_hook_install_allowed = false;
std::atomic<bool> g_hook_transition = false;
std::atomic<unsigned int> g_present_active_calls = 0;
std::atomic<unsigned int> g_resize_active_calls = 0;
// The bundled MinHook library frees a trampoline from MH_RemoveHook and all
// trampolines from MH_Uninitialize.  A detour can already have loaded its
// original pointer when teardown starts, so neither operation is safe without
// a true execution-drain primitive (this fork has none).  Keep the hook state
// and trampolines for process lifetime; the DLL itself is pinned in DllMain.
std::atomic<bool> g_hooks_retained_process_lifetime = false;

std::mutex g_mutex;
std::mutex g_hook_mutex;

void IncrementDiagnostic(std::atomic<unsigned long long>& counter) {
#if !defined(SPATCH_FINAL_RELEASE)
    counter.fetch_add(1, std::memory_order_relaxed);
#else
    (void)counter;
#endif
}

void MarkReplacementUnavailable() noexcept {
    g_successful_pass.store(false, std::memory_order_release);
    g_successful_swapchain.store(nullptr, std::memory_order_release);
    g_successful_device.store(nullptr, std::memory_order_release);
    g_successful_generation.store(0, std::memory_order_release);
}

void PublishReplacementSuccess(IDXGISwapChain* swapchain,
                               ID3D11Device* device,
                               unsigned long long generation) noexcept {
    g_successful_device.store(device, std::memory_order_relaxed);
    g_successful_swapchain.store(swapchain, std::memory_order_relaxed);
    g_successful_generation.store(generation, std::memory_order_relaxed);
    g_successful_pass.store(swapchain != nullptr && device != nullptr && generation != 0,
                            std::memory_order_release);
}

void AdvanceResourceGeneration() noexcept {
    g_resource_generation.fetch_add(1, std::memory_order_acq_rel);
    MarkReplacementUnavailable();
}

bool PublishReplacementSuccessIfCurrent(IDXGISwapChain* swapchain,
                                        ID3D11Device* device,
                                        unsigned long long generation) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (generation == 0 ||
            generation != g_resource_generation.load(std::memory_order_acquire)) {
            return false;
        }
        PublishReplacementSuccess(swapchain, device, generation);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadHookBytes(const void* target,
                   std::array<std::uint8_t, kOwnedHookPatchBytes>& bytes) noexcept {
    if (target == nullptr) {
        return false;
    }
    __try {
        std::memcpy(bytes.data(), target, bytes.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bytes.fill(0);
        return false;
    }
}

bool IsExecutableMemory(const void* address, DWORD required_type = 0) noexcept {
    if (address == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT ||
        (required_type != 0 && memory.Type != required_type) ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (memory.Protect & 0xFFU) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool IsAddressInNamedModule(const void* address, const wchar_t* expected_name) noexcept {
    if (address == nullptr || expected_name == nullptr) {
        return false;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address),
                            &module) ||
        module == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return false;
    }
    const wchar_t* const slash = std::wcsrchr(path, L'\\');
    const wchar_t* const file_name = slash == nullptr ? path : slash + 1;
    return _wcsicmp(file_name, expected_name) == 0;
}

bool ReadRelativeJump(const void* source,
                      void*& destination,
                      std::array<std::uint8_t, kOwnedHookPatchBytes>& bytes,
                      bool validate_destination_memory = true) noexcept {
    destination = nullptr;
    if (!ReadHookBytes(source, bytes) || bytes[0] != 0xE9) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    const std::int64_t destination_address =
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(source) + 5) +
        static_cast<std::int64_t>(displacement);
    if (destination_address <= 0) {
        return false;
    }
    destination = reinterpret_cast<void*>(static_cast<std::uintptr_t>(destination_address));
    return !validate_destination_memory || IsExecutableMemory(destination);
}

RedirectChainCapture ResolveCompatibleHookTarget(void* vtable_target) noexcept {
    RedirectChainCapture capture;
    capture.vtable_target = vtable_target;
    capture.hook_target = vtable_target;
    if (!IsAddressInNamedModule(vtable_target, L"dxgi.dll")) {
        return capture;
    }

    void* relay = nullptr;
    if (!ReadRelativeJump(vtable_target, relay, capture.vtable_bytes) ||
        !IsExecutableMemory(relay, MEM_PRIVATE)) {
        return capture;
    }
    void* overlay_target = nullptr;
    if (!ReadRelativeJump(relay, overlay_target, capture.relay_bytes) ||
        !IsAddressInNamedModule(overlay_target, L"gameoverlayrenderer64.dll")) {
        return capture;
    }

    capture.relay = relay;
    capture.hook_target = overlay_target;
    capture.steam_overlay = true;
    return capture;
}

bool RedirectChainIntact(const AtomicRedirectChainSnapshot& chain) noexcept {
    if (!chain.active.load(std::memory_order_acquire)) {
        return true;
    }
    void* const expected_vtable_target =
        chain.vtable_target.load(std::memory_order_acquire);
    void* const expected_relay = chain.relay.load(std::memory_order_acquire);
    void* const expected_hook_target = chain.hook_target.load(std::memory_order_acquire);
    std::array<std::uint8_t, kOwnedHookPatchBytes> vtable_bytes{};
    std::array<std::uint8_t, kOwnedHookPatchBytes> relay_bytes{};
    void* live_relay = nullptr;
    void* live_hook_target = nullptr;
    return ReadRelativeJump(
               expected_vtable_target, live_relay, vtable_bytes, false) &&
           live_relay == expected_relay && chain.vtable_bytes.Matches(vtable_bytes) &&
           ReadRelativeJump(expected_relay, live_hook_target, relay_bytes, false) &&
           live_hook_target == expected_hook_target && chain.relay_bytes.Matches(relay_bytes);
}

void PublishRedirectChain(AtomicRedirectChainSnapshot& destination,
                          const RedirectChainCapture& capture) noexcept {
    destination.Store(capture);
}

// The bundled SDmodding MinHook writes either a five-byte E9 at the entry or
// an E9 in the five-byte hotpatch area plus EB F9 at the entry. In both cases
// that relative jump must land on MinHook's x64 FF 25 relay, whose embedded
// absolute destination must be this exact SPatch detour. Merely snapshotting
// whatever bytes happen to exist after MH_CreateHook can otherwise bless a
// concurrently installed overlay stub as our own.
bool IsOwnedMinHookRedirect(const void* target, const void* detour) noexcept {
    if (target == nullptr || detour == nullptr) {
        return false;
    }

    __try {
        const auto* const entry = static_cast<const std::uint8_t*>(target);
        const std::uint8_t* relative_jump = nullptr;
        if (entry[0] == 0xE9) {
            relative_jump = entry;
        } else if (entry[0] == 0xEB && entry[1] == 0xF9 && entry[-5] == 0xE9) {
            relative_jump = entry - 5;
        } else {
            return false;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, relative_jump + 1, sizeof(displacement));
        const std::int64_t relay_address =
            static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(relative_jump) + 5) +
            static_cast<std::int64_t>(displacement);
        if (relay_address <= 0) {
            return false;
        }
        const auto* const relay =
            reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(relay_address));
        std::uint32_t indirect_displacement = 1;
        std::memcpy(&indirect_displacement, relay + 2, sizeof(indirect_displacement));
        if (relay[0] != 0xFF || relay[1] != 0x25 || indirect_displacement != 0) {
            return false;
        }

        const void* relay_destination = nullptr;
        std::memcpy(&relay_destination, relay + 6, sizeof(relay_destination));
        return relay_destination == detour;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool OwnedHookBytesIntact() noexcept {
    if (!g_owned_hook_bytes_valid.load(std::memory_order_acquire)) {
        return false;
    }
    const void* const present_target =
        g_present_hook_target.load(std::memory_order_acquire);
    const void* const resize_target =
        g_resize_buffers_hook_target.load(std::memory_order_acquire);
    std::array<std::uint8_t, kOwnedHookPatchBytes> present_bytes{};
    std::array<std::uint8_t, kOwnedHookPatchBytes> resize_bytes{};
    return ReadHookBytes(present_target, present_bytes) &&
           ReadHookBytes(resize_target, resize_bytes) &&
           g_present_hook_bytes.Matches(present_bytes) &&
           g_resize_buffers_hook_bytes.Matches(resize_bytes) &&
           RedirectChainIntact(g_present_redirect_chain) &&
           RedirectChainIntact(g_resize_redirect_chain) &&
           IsOwnedMinHookRedirect(present_target,
                                  reinterpret_cast<const void*>(&DetourPresent)) &&
           IsOwnedMinHookRedirect(resize_target,
                                  reinterpret_cast<const void*>(&DetourResizeBuffers));
}

bool ValidateOwnedHookBytes() noexcept {
    if (OwnedHookBytesIntact()) {
        return true;
    }
    g_owned_hook_bytes_valid.store(false, std::memory_order_release);
    MarkReplacementUnavailable();
    return false;
}

void MarkHookDiscoveryNeeded() noexcept {
    g_hook_discovery_needed.store(true, std::memory_order_release);
    g_hook_next_probe_tick.store(0, std::memory_order_relaxed);
}

void MarkHookDiscoveryComplete(unsigned long long now) noexcept {
    g_hook_discovery_needed.store(false, std::memory_order_release);
    g_hook_next_probe_tick.store(now + 1000, std::memory_order_relaxed);
}

constexpr unsigned long long kRejectedTargetRetryMilliseconds = 10000;

bool IsRejectedTargetPair(IDXGISwapChain* swapchain,
                          void* present_target,
                          void* resize_target) noexcept {
    const unsigned long long retry_after =
        g_rejected_retry_after_tick.load(std::memory_order_acquire);
    return retry_after != 0 && GetTickCount64() < retry_after &&
           g_rejected_swapchain.load(std::memory_order_acquire) == swapchain &&
           g_rejected_present_target.load(std::memory_order_acquire) == present_target &&
           g_rejected_resize_target.load(std::memory_order_acquire) == resize_target;
}

void ClearRejectedTargetPair() noexcept {
    g_rejected_present_target.store(nullptr, std::memory_order_release);
    g_rejected_resize_target.store(nullptr, std::memory_order_release);
    g_rejected_swapchain.store(nullptr, std::memory_order_release);
    g_rejected_retry_after_tick.store(0, std::memory_order_release);
    g_rejected_log_emitted.store(false, std::memory_order_release);
}

void UpdateRejectedTargetPair(IDXGISwapChain* swapchain,
                              void* present_target,
                              void* resize_target) noexcept {
    MarkReplacementUnavailable();
    g_rejected_swapchain.store(swapchain, std::memory_order_release);
    g_rejected_present_target.store(present_target, std::memory_order_release);
    g_rejected_resize_target.store(resize_target, std::memory_order_release);
    const unsigned long long retry_after =
        GetTickCount64() + kRejectedTargetRetryMilliseconds;
    g_rejected_retry_after_tick.store(retry_after, std::memory_order_release);
    // MaybeInstallHooks is called from render-path detours.  The rejected
    // pair cache prevents duplicate diagnostics, but it must also suppress
    // the expensive COM/vtable/PE probe until the same bounded retry window
    // expires; otherwise an unsupported overlay still costs work every frame.
    g_hook_retry_after_tick.store(retry_after, std::memory_order_release);
}

void ForgetRejectedPairIfChanged(IDXGISwapChain* swapchain,
                                 void* present_target,
                                 void* resize_target) noexcept {
    const void* const rejected_swapchain =
        g_rejected_swapchain.load(std::memory_order_acquire);
    const void* const rejected_present =
        g_rejected_present_target.load(std::memory_order_acquire);
    const void* const rejected_resize =
        g_rejected_resize_target.load(std::memory_order_acquire);
    if ((rejected_swapchain != nullptr || rejected_present != nullptr || rejected_resize != nullptr) &&
        (rejected_swapchain != swapchain || rejected_present != present_target ||
         rejected_resize != resize_target)) {
        ClearRejectedTargetPair();
        g_hook_retry_after_tick.store(0, std::memory_order_relaxed);
    }
}

bool HasPartialHookOwnership() noexcept {
    const bool owns_present =
        g_present_hook_target.load(std::memory_order_acquire) != nullptr;
    const bool owns_resize =
        g_resize_buffers_hook_target.load(std::memory_order_acquire) != nullptr;
    return owns_present != owns_resize;
}

bool EnterHookCall(std::atomic<unsigned int>& active_calls) noexcept {
    for (;;) {
        while (g_hook_transition.load(std::memory_order_acquire)) {
            SwitchToThread();
        }
        active_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!g_hook_transition.load(std::memory_order_acquire)) {
            return true;
        }
        active_calls.fetch_sub(1, std::memory_order_release);
    }
}

void LeaveHookCall(std::atomic<unsigned int>& active_calls) noexcept {
    active_calls.fetch_sub(1, std::memory_order_release);
}

class HookTransitionGuard {
public:
    HookTransitionGuard() noexcept {
        bool expected = false;
        active_ = g_hook_transition.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
        if (!active_) {
            return;
        }
        for (unsigned int attempt = 0; attempt < 2000; ++attempt) {
            if (g_present_active_calls.load(std::memory_order_acquire) == 0 &&
                g_resize_active_calls.load(std::memory_order_acquire) == 0) {
                return;
            }
            SwitchToThread();
        }
        g_hook_transition.store(false, std::memory_order_release);
        active_ = false;
    }

    ~HookTransitionGuard() {
        if (active_) {
            g_hook_transition.store(false, std::memory_order_release);
        }
    }

    HookTransitionGuard(const HookTransitionGuard&) = delete;
    HookTransitionGuard& operator=(const HookTransitionGuard&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    bool active_ = false;
};

// blendFactor removed: it was declared in the cbuffer but never read by any
// shader pass. The struct is now 32 bytes, still a valid cbuffer size.
struct ConstantBufferData {
    float rt_metrics[4];
    float subsample_indices[4];
};

struct PipelineState {
    // D3D11 resources are owned by the device that created them, and the
    // backbuffer-sized graph belongs to one specific swapchain. Keep strong
    // identity references so pointer reuse after a device reset cannot make an
    // old resource graph look compatible with a new render path.
    ComPtr<ID3D11Device> owner_device;
    ComPtr<IDXGISwapChain> owner_swapchain;
    ComPtr<ID3D11VertexShader> edge_vs;
    ComPtr<ID3D11PixelShader> edge_ps;
    ComPtr<ID3D11VertexShader> blend_vs;
    ComPtr<ID3D11PixelShader> blend_ps;
    ComPtr<ID3D11VertexShader> neighborhood_vs;
    ComPtr<ID3D11PixelShader> neighborhood_ps;
    ComPtr<ID3D11Buffer> constant_buffer;
    ComPtr<ID3D11BlendState> blend_state;
    ComPtr<ID3D11DepthStencilState> depth_state;
    ComPtr<ID3D11DepthStencilState> edge_stencil_state;
    ComPtr<ID3D11DepthStencilState> weight_stencil_state;
    ComPtr<ID3D11RasterizerState> rasterizer_state;
    // SMAA requires explicit linear+clamp and point+clamp samplers. The inline
    // SamplerState initialisers in the SMAA HLSL source are silently ignored by
    // D3DCompile outside of the FX framework, so we create them here. FXC strips
    // the unused sampler from each pixel shader and assigns the one live sampler
    // to s0; each pass therefore binds its own required sampler explicitly.
    ComPtr<ID3D11SamplerState> linear_sampler;
    ComPtr<ID3D11SamplerState> point_sampler;
    ComPtr<ID3D11Texture2D> area_texture;
    ComPtr<ID3D11ShaderResourceView> area_srv;
    ComPtr<ID3D11Texture2D> search_texture;
    ComPtr<ID3D11ShaderResourceView> search_srv;
    ComPtr<ID3D11Texture2D> source_texture;
    ComPtr<ID3D11ShaderResourceView> source_linear_srv;
    ComPtr<ID3D11ShaderResourceView> source_srgb_srv;
    // A multisampled backbuffer cannot be restored from the single-sample
    // resolved SMAA input. Allocate this only for that uncommon path.
    ComPtr<ID3D11Texture2D> multisample_source_texture;
    ComPtr<ID3D11Texture2D> output_backbuffer;
    ComPtr<ID3D11RenderTargetView> direct_output_srgb_rtv;
    ComPtr<ID3D11Texture2D> output_texture;
    ComPtr<ID3D11RenderTargetView> output_srgb_rtv;
    ComPtr<ID3D11Texture2D> edges_texture;
    ComPtr<ID3D11RenderTargetView> edges_rtv;
    ComPtr<ID3D11ShaderResourceView> edges_srv;
    ComPtr<ID3D11Texture2D> blend_texture;
    ComPtr<ID3D11RenderTargetView> blend_rtv;
    ComPtr<ID3D11ShaderResourceView> blend_srv;
    ComPtr<ID3D11Texture2D> stencil_texture;
    ComPtr<ID3D11DepthStencilView> stencil_dsv;
    DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    bool shaders_ready = false;
};

PipelineState g_pipeline;

struct ScopedPipelineState;

// ApplySmaa is protected by an outer SEH fail-open boundary. Under /EHsc, C++
// stack owners are not unwound when a stale overlay/device pointer faults, so
// every retained COM reference and recovery latch lives in this out-of-stack,
// thread-local record until CleanupActiveSmaaPass releases it explicitly.
struct ActiveSmaaPass {
    std::mutex* mutex = nullptr;
    ScopedPipelineState* pipeline_state = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> context_device;
    ComPtr<ID3D11Device> swapchain_device;
    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11Texture2D> source_snapshot;
    ComPtr<ID3D11DeviceContext> mapped_context;
    bool mutex_locked = false;
    bool constant_buffer_mapped = false;
    bool resource_graph_uncertain = false;
    bool source_snapshot_valid = false;
    bool backbuffer_write_started = false;
};
static_assert(std::is_nothrow_default_constructible_v<ActiveSmaaPass>);

alignas(ActiveSmaaPass) thread_local std::array<unsigned char, sizeof(ActiveSmaaPass)>
    g_active_smaa_pass_storage{};
thread_local ActiveSmaaPass* g_active_smaa_pass = nullptr;
thread_local bool g_active_smaa_pass_storage_usable = true;

thread_local bool g_present_reentry = false;
HMODULE g_d3dcompiler_module = nullptr;
D3DCompileFn g_d3d_compile = nullptr;

struct ShaderBytecodeCache {
    ComPtr<ID3DBlob> edge_vs;
    ComPtr<ID3DBlob> edge_ps;
    ComPtr<ID3DBlob> blend_vs;
    ComPtr<ID3DBlob> blend_ps;
    ComPtr<ID3DBlob> neighborhood_vs;
    ComPtr<ID3DBlob> neighborhood_ps;
    int preset = -1;
    bool attempted = false;
    bool ready = false;
};

ShaderBytecodeCache g_shader_bytecode;

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
T* ReadSlot(std::uintptr_t slot) noexcept {
    if (slot == 0) {
        return nullptr;
    }
    __try {
        return *reinterpret_cast<T**>(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

template <typename T>
T* RetainSlotValue(std::uintptr_t slot) noexcept {
    if (slot == 0) {
        return nullptr;
    }
    __try {
        T* const value = *reinterpret_cast<T**>(slot);
        if (value != nullptr) {
            value->AddRef();
        }
        return value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool IsOurSwapChain(IDXGISwapChain* swapchain) {
    IDXGISwapChain* const current = ReadSlot<IDXGISwapChain>(g_swapchain_slot);
    return current != nullptr && current == swapchain;
}

bool ReadSwapChainTargets(IDXGISwapChain* swapchain,
                          void*& present_target,
                          void*& resize_buffers_target) noexcept {
    present_target = nullptr;
    resize_buffers_target = nullptr;
    if (swapchain == nullptr) {
        return false;
    }

    __try {
        void** const vtable = *reinterpret_cast<void***>(swapchain);
        if (vtable == nullptr) {
            return false;
        }
        present_target = vtable[kPresentVtableIndex];
        resize_buffers_target = vtable[kResizeBuffersVtableIndex];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        present_target = nullptr;
        resize_buffers_target = nullptr;
        return false;
    }
    return present_target != nullptr && resize_buffers_target != nullptr;
}

bool CurrentSwapChainTargetsMatchOwnedPath() noexcept {
    IDXGISwapChain* const swapchain = ReadSlot<IDXGISwapChain>(g_swapchain_slot);
    void* present_target = nullptr;
    void* resize_target = nullptr;
    if (swapchain == nullptr ||
        !ReadSwapChainTargets(swapchain, present_target, resize_target)) {
        return false;
    }
    return present_target == g_present_vtable_target.load(std::memory_order_acquire) &&
           resize_target == g_resize_vtable_target.load(std::memory_order_acquire);
}

// g_mutex must be held while either reset helper is called.
void ResetSizedResourcesLocked() {
    AdvanceResourceGeneration();
    g_pipeline.source_linear_srv.Reset();
    g_pipeline.source_srgb_srv.Reset();
    g_pipeline.source_texture.Reset();
    g_pipeline.multisample_source_texture.Reset();
    g_pipeline.direct_output_srgb_rtv.Reset();
    g_pipeline.output_backbuffer.Reset();
    g_pipeline.output_srgb_rtv.Reset();
    g_pipeline.output_texture.Reset();
    g_pipeline.edges_rtv.Reset();
    g_pipeline.edges_srv.Reset();
    g_pipeline.edges_texture.Reset();
    g_pipeline.blend_rtv.Reset();
    g_pipeline.blend_srv.Reset();
    g_pipeline.blend_texture.Reset();
    g_pipeline.stencil_dsv.Reset();
    g_pipeline.stencil_texture.Reset();
    g_pipeline.width = 0;
    g_pipeline.height = 0;
    g_pipeline.backbuffer_format = DXGI_FORMAT_UNKNOWN;
    g_sized_resources_ready.store(false);
    g_sized_retry_after_tick.store(0, std::memory_order_relaxed);
    g_width.store(0);
    g_height.store(0);
}

// Reset the complete graph when its creating device changes. Retaining only
// dimensions and format is insufficient: D3D11 objects from one device cannot
// be rebound on another device even when the new swapchain looks identical.
void ResetPipelineResourcesLocked(ID3D11Device* owner_device) {
    AdvanceResourceGeneration();
    // Retain the incoming device before releasing the old graph. On retry the
    // incoming pointer can be the same object held by g_pipeline.
    ComPtr<ID3D11Device> retained_owner = owner_device;
    g_pipeline = {};
    g_pipeline.owner_device = std::move(retained_owner);
    g_resources_ready.store(false);
    g_sized_resources_ready.store(false);
    g_static_retry_after_tick.store(0, std::memory_order_relaxed);
    g_sized_retry_after_tick.store(0, std::memory_order_relaxed);
    g_width.store(0);
    g_height.store(0);
}

// g_hook_mutex must be held.  The bundled MinHook library has no disable-only
// API and MH_RemoveHook frees the trampoline.  Retain the target instead of
// freeing it: every in-flight detour and every later fail-open call continues
// to have a valid original trampoline for the rest of the process lifetime.
bool RetainOwnedHookTarget(std::atomic<void*>& stored_target, const char* name) {
    void* const target = stored_target.load(std::memory_order_relaxed);
    if (target == nullptr) {
        return true;
    }

    MarkReplacementUnavailable();
    const bool was_retained =
        g_hooks_retained_process_lifetime.exchange(true, std::memory_order_acq_rel);
    if (!was_retained) {
        log::InfoF("smaa_hook_retained_process_lifetime name=%s target=0x%p", name, target);
    }
    // Deliberately leave stored_target and the MinHook trampoline untouched.
    return true;
}

// DXGI swap-chain methods are discovered from a live COM vtable rather than a
// fixed RVA in the game image, so Hooks.cpp's process-image guard cannot cover
// them. Resolve the owning PE from each method address and compare the live
// decoder window with that module's on-disk image immediately before creating
// the detour. A private/unknown/modified trampoline is rejected instead of
// chaining blindly into another overlay or a freed allocation.
bool VerifyHookTarget(const void* target,
                      const char* name,
                      hook_guard::Guard& guard,
                      bool report_failure = true) {
    hook_guard::Result initialize_result;
    if (!guard.InitializeForAddress(target, &initialize_result)) {
        if (report_failure) {
            log::ErrorF("smaa_hook_target_rejected name=%s reason=owner_module status=%s "
                        "pe_status=%s win32=%u target=0x%p",
                        name,
                        hook_guard::StatusName(initialize_result.status),
                        hook_guard::PeStatusName(initialize_result.pe_status),
                        initialize_result.win32_error,
                        target);
        }
        return false;
    }

    const hook_guard::Result verify_result = guard.Verify(
        target, hook_guard::kMinHookTargetVerificationBytes);
    if (!verify_result.verified()) {
        if (report_failure) {
            log::ErrorF("smaa_hook_target_rejected name=%s reason=unexpected_bytes status=%s "
                        "pe_status=%s rva=0x%X mismatch=%zu win32=%u target=0x%p",
                        name,
                        hook_guard::StatusName(verify_result.status),
                        hook_guard::PeStatusName(verify_result.pe_status),
                        verify_result.target_rva,
                        verify_result.mismatch_offset,
                        verify_result.win32_error,
                        target);
        }
        return false;
    }
    return true;
}

void RefreshHookOwnershipState() {
    const bool owns_present =
        g_present_hook_target.load(std::memory_order_acquire) != nullptr;
    const bool owns_resize =
        g_resize_buffers_hook_target.load(std::memory_order_acquire) != nullptr;
    const bool owns_hook = owns_present || owns_resize;
    g_any_hook_retained.store(owns_hook, std::memory_order_release);
    if (!owns_present || !owns_resize) {
        MarkReplacementUnavailable();
    }
}

bool HasCompleteHookPair() noexcept {
    return g_present_hook_target.load(std::memory_order_acquire) != nullptr &&
           g_resize_buffers_hook_target.load(std::memory_order_acquire) != nullptr;
}

bool CaptureNewHookBytesLocked(void* target,
                               const void* detour,
                               AtomicHookByteSnapshot& snapshot,
                               std::atomic<bool>& captured) noexcept {
    std::array<std::uint8_t, kOwnedHookPatchBytes> bytes{};
    if (target == nullptr || !ReadHookBytes(target, bytes) ||
        !IsOwnedMinHookRedirect(target, detour)) {
        captured.store(false, std::memory_order_release);
        return false;
    }
    snapshot.Store(bytes);
    captured.store(true, std::memory_order_release);
    return true;
}

void PublishOwnedHookBytesValidityLocked() noexcept {
    const bool complete =
        g_present_hook_bytes_captured.load(std::memory_order_acquire) &&
        g_resize_hook_bytes_captured.load(std::memory_order_acquire) &&
        g_present_vtable_target.load(std::memory_order_acquire) != nullptr &&
        g_resize_vtable_target.load(std::memory_order_acquire) != nullptr &&
        RedirectChainIntact(g_present_redirect_chain) &&
        RedirectChainIntact(g_resize_redirect_chain);
    g_owned_hook_bytes_valid.store(complete, std::memory_order_release);
}

bool CompileShader(const std::string& source,
                   const char* entry_point,
                   const char* profile,
                   ID3DBlob** shader_blob) {
    if (g_d3d_compile == nullptr) {
        if (g_d3dcompiler_module == nullptr) {
            g_d3dcompiler_module = LoadSystemLibrary(L"D3DCompiler_47.dll");
        }
        if (g_d3dcompiler_module != nullptr) {
            g_d3d_compile = reinterpret_cast<D3DCompileFn>(
                GetProcAddress(g_d3dcompiler_module, "D3DCompile"));
        }
        if (g_d3d_compile == nullptr) {
            log::Error("smaa_d3dcompiler_load_fail");
            return false;
        }
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = g_d3d_compile(source.data(),
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

// Shader compilation is CPU-only and costs roughly 0.1-0.2 seconds on the
// tested game system. Do it on SPatch's bootstrap worker before any DXGI hook
// can run, then reuse the bytecode for every device/reset. Present must never
// synchronously invoke D3DCompile.
bool PrepareShaderBytecodeLocked() {
    const int preset = g_preset.load(std::memory_order_acquire);
    if (g_shader_bytecode.attempted && g_shader_bytecode.preset == preset) {
        return g_shader_bytecode.ready;
    }

    const std::string source = BuildShaderSource();
    ShaderBytecodeCache pending;
    pending.preset = preset;
    pending.attempted = true;
    if (!CompileShader(source, "EdgeVS", "vs_5_0", pending.edge_vs.GetAddressOf()) ||
        !CompileShader(source, "EdgePS", "ps_5_0", pending.edge_ps.GetAddressOf()) ||
        !CompileShader(source, "BlendVS", "vs_5_0", pending.blend_vs.GetAddressOf()) ||
        !CompileShader(source, "BlendPS", "ps_5_0", pending.blend_ps.GetAddressOf()) ||
        !CompileShader(source,
                       "NeighborhoodVS",
                       "vs_5_0",
                       pending.neighborhood_vs.GetAddressOf()) ||
        !CompileShader(source,
                       "NeighborhoodPS",
                       "ps_5_0",
                       pending.neighborhood_ps.GetAddressOf())) {
        pending = {};
        pending.preset = preset;
        pending.attempted = true;
        g_shader_bytecode = std::move(pending);
        log::ErrorF("smaa_shader_precompile_fail preset=%d", preset);
        return false;
    }

    pending.ready = true;
    g_shader_bytecode = std::move(pending);
    log::InfoF("smaa_shader_precompile_ready preset=%d", preset);
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
    if (g_pipeline.owner_device.Get() != device) {
        ID3D11Device* const previous_device = g_pipeline.owner_device.Get();
        log::InfoF("smaa_device_changed previous=0x%p current=0x%p",
                   previous_device,
                   device);
        ResetPipelineResourcesLocked(device);
    }

    if (g_pipeline.shaders_ready) {
        g_resources_ready.store(true);
        return true;
    }

    const unsigned long long now = GetTickCount64();
    const unsigned long long retry_after =
        g_static_retry_after_tick.load(std::memory_order_relaxed);
    if (retry_after != 0 && now < retry_after) {
        return false;
    }

    const bool result = [&]() -> bool {
    if (!g_shader_bytecode.ready ||
        g_shader_bytecode.preset != g_preset.load(std::memory_order_acquire)) {
        log::Error("smaa_static_resources_fail reason=shader_bytecode_unavailable");
        return false;
    }

    if (FAILED(device->CreateVertexShader(g_shader_bytecode.edge_vs->GetBufferPointer(),
                                          g_shader_bytecode.edge_vs->GetBufferSize(),
                                          nullptr,
                                          g_pipeline.edge_vs.GetAddressOf()))) {
        return false;
    }

    if (FAILED(device->CreatePixelShader(g_shader_bytecode.edge_ps->GetBufferPointer(),
                                         g_shader_bytecode.edge_ps->GetBufferSize(),
                                         nullptr,
                                         g_pipeline.edge_ps.GetAddressOf()))) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(g_shader_bytecode.blend_vs->GetBufferPointer(),
                                          g_shader_bytecode.blend_vs->GetBufferSize(),
                                          nullptr,
                                          g_pipeline.blend_vs.GetAddressOf()))) {
        return false;
    }

    if (FAILED(device->CreatePixelShader(g_shader_bytecode.blend_ps->GetBufferPointer(),
                                         g_shader_bytecode.blend_ps->GetBufferSize(),
                                         nullptr,
                                         g_pipeline.blend_ps.GetAddressOf()))) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(g_shader_bytecode.neighborhood_vs->GetBufferPointer(),
                                          g_shader_bytecode.neighborhood_vs->GetBufferSize(),
                                          nullptr,
                                          g_pipeline.neighborhood_vs.GetAddressOf()))) {
        return false;
    }

    if (FAILED(device->CreatePixelShader(g_shader_bytecode.neighborhood_ps->GetBufferPointer(),
                                         g_shader_bytecode.neighborhood_ps->GetBufferSize(),
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

    // Canonical SMAA writes stencil only for pixels that survive the edge
    // shader's discard, then restricts the expensive weight search to those
    // pixels. This changes no color result and avoids running BlendPS over the
    // large non-edge majority of the frame.
    D3D11_DEPTH_STENCIL_DESC edge_stencil_desc{};
    edge_stencil_desc.DepthEnable = FALSE;
    edge_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    edge_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    edge_stencil_desc.StencilEnable = TRUE;
    edge_stencil_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    edge_stencil_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    edge_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    edge_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    edge_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    edge_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    edge_stencil_desc.BackFace = edge_stencil_desc.FrontFace;
    if (FAILED(device->CreateDepthStencilState(
            &edge_stencil_desc, g_pipeline.edge_stencil_state.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC weight_stencil_desc = edge_stencil_desc;
    weight_stencil_desc.StencilWriteMask = 0;
    weight_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    weight_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    weight_stencil_desc.BackFace = weight_stencil_desc.FrontFace;
    if (FAILED(device->CreateDepthStencilState(
            &weight_stencil_desc, g_pipeline.weight_stencil_state.GetAddressOf()))) {
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

    // LinearSampler matches SMAA's MIN_MAG_LINEAR_MIP_POINT + Clamp.
    // PointSampler matches SMAA's MIN_MAG_MIP_POINT + Clamp.
    // These are created explicitly because the inline SamplerState initialisers
    // in the embedded SMAA HLSL are ignored by D3DCompile outside of the FX
    // framework. Optimized pixel shaders compact their one live sampler to s0;
    // each pass binds the corresponding state there immediately before drawing.
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
    }();

    if (!result) {
        // Release partial device resources and back off briefly. Shader
        // bytecode is already precompiled and is never regenerated here.
        ResetPipelineResourcesLocked(device);
        g_static_retry_after_tick.store(GetTickCount64() + 1000, std::memory_order_relaxed);
    } else {
        g_static_retry_after_tick.store(0, std::memory_order_relaxed);
    }
    return result;
}

bool EnsureSizedResources(ID3D11Device* device, IDXGISwapChain* swapchain) {
    ActiveSmaaPass* const active_pass = g_active_smaa_pass;
    if (active_pass == nullptr) {
        return false;
    }
    if (g_pipeline.owner_device.Get() != device) {
        // EnsureStaticResources normally establishes this invariant. Fail
        // closed if a future caller reaches this function out of order.
        ResetPipelineResourcesLocked(device);
        return false;
    }

    if (g_pipeline.owner_swapchain.Get() != swapchain) {
        IDXGISwapChain* const previous_swapchain = g_pipeline.owner_swapchain.Get();
        log::InfoF("smaa_swapchain_changed previous=0x%p current=0x%p",
                   previous_swapchain,
                   swapchain);
        ResetSizedResourcesLocked();
        g_pipeline.owner_swapchain = swapchain;
    }

    const unsigned long long now = GetTickCount64();
    const unsigned long long retry_after =
        g_sized_retry_after_tick.load(std::memory_order_relaxed);
    if (retry_after != 0 && now < retry_after) {
        return false;
    }
    const auto fail_with_backoff = []() {
        ResetSizedResourcesLocked();
        g_sized_retry_after_tick.store(GetTickCount64() + 1000, std::memory_order_relaxed);
        return false;
    };

    active_pass->backbuffer.Reset();
    if (FAILED(swapchain->GetBuffer(
            0, IID_PPV_ARGS(active_pass->backbuffer.GetAddressOf())))) {
        return fail_with_backoff();
    }

    D3D11_TEXTURE2D_DESC bb_desc{};
    active_pass->backbuffer->GetDesc(&bb_desc);
    const DXGI_FORMAT typeless_format = SmaaTypelessFormat(bb_desc.Format);
    const DXGI_FORMAT linear_format = SmaaLinearFormat(bb_desc.Format);
    const DXGI_FORMAT srgb_format = SmaaSrgbFormat(bb_desc.Format);
    const UINT width = bb_desc.Width;
    const UINT height = bb_desc.Height;
    // A minimized/resizing swap chain can briefly expose a zero-sized
    // backbuffer. Do not create resources or feed zero into the reciprocal
    // constant-buffer fields; fail open and let the next Present retry.
    if (width == 0 || height == 0 || typeless_format == DXGI_FORMAT_UNKNOWN ||
        linear_format == DXGI_FORMAT_UNKNOWN || srgb_format == DXGI_FORMAT_UNKNOWN) {
        return fail_with_backoff();
    }

    if (g_pipeline.owner_device.Get() == device &&
        g_pipeline.owner_swapchain.Get() == swapchain &&
        g_pipeline.width == width && g_pipeline.height == height &&
        g_pipeline.backbuffer_format == bb_desc.Format && g_pipeline.source_texture != nullptr &&
        g_pipeline.source_linear_srv != nullptr && g_pipeline.source_srgb_srv != nullptr &&
        (bb_desc.SampleDesc.Count == 1 ||
         g_pipeline.multisample_source_texture != nullptr) &&
        g_pipeline.output_backbuffer.Get() == active_pass->backbuffer.Get() &&
        (g_pipeline.direct_output_srgb_rtv != nullptr ||
         (g_pipeline.output_texture != nullptr && g_pipeline.output_srgb_rtv != nullptr)) &&
        g_pipeline.edges_texture != nullptr && g_pipeline.blend_texture != nullptr &&
        g_pipeline.stencil_texture != nullptr && g_pipeline.stencil_dsv != nullptr) {
        g_sized_resources_ready.store(true);
        g_width.store(width);
        g_height.store(height);
        g_sized_retry_after_tick.store(0, std::memory_order_relaxed);
        return true;
    }

    ResetSizedResourcesLocked();

    D3D11_TEXTURE2D_DESC source_desc{};
    source_desc.Width = width;
    source_desc.Height = height;
    source_desc.MipLevels = 1;
    source_desc.ArraySize = 1;
    source_desc.Format = typeless_format;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_DEFAULT;
    source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&source_desc, nullptr, g_pipeline.source_texture.GetAddressOf()))) {
        return fail_with_backoff();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC source_srv_desc{};
    source_srv_desc.Format = linear_format;
    source_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    source_srv_desc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(
            g_pipeline.source_texture.Get(),
            &source_srv_desc,
            g_pipeline.source_linear_srv.GetAddressOf()))) {
        return fail_with_backoff();
    }
    source_srv_desc.Format = srgb_format;
    if (FAILED(device->CreateShaderResourceView(
            g_pipeline.source_texture.Get(),
            &source_srv_desc,
            g_pipeline.source_srgb_srv.GetAddressOf()))) {
        return fail_with_backoff();
    }

    if (bb_desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC multisample_source_desc = bb_desc;
        multisample_source_desc.Usage = D3D11_USAGE_DEFAULT;
        multisample_source_desc.BindFlags = 0;
        multisample_source_desc.CPUAccessFlags = 0;
        multisample_source_desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(
                &multisample_source_desc,
                nullptr,
                g_pipeline.multisample_source_texture.GetAddressOf()))) {
            return fail_with_backoff();
        }
    }

    D3D11_RENDER_TARGET_VIEW_DESC output_rtv_desc{};
    output_rtv_desc.Format = srgb_format;
    if (bb_desc.SampleDesc.Count > 1) {
        output_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
        output_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        output_rtv_desc.Texture2D.MipSlice = 0;
    }
    // Feature-level 11 implementations may permit the UNORM swap-chain buffer
    // to be viewed as its sRGB twin. Prefer that zero-copy path, but retain a
    // typeless intermediate fallback for runtimes that reject the cast.
    g_pipeline.output_backbuffer = active_pass->backbuffer;
    if (FAILED(device->CreateRenderTargetView(
            active_pass->backbuffer.Get(),
            &output_rtv_desc,
            g_pipeline.direct_output_srgb_rtv.GetAddressOf()))) {
        D3D11_TEXTURE2D_DESC output_desc = source_desc;
        output_desc.SampleDesc = bb_desc.SampleDesc;
        output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(
                &output_desc, nullptr, g_pipeline.output_texture.GetAddressOf())) ||
            FAILED(device->CreateRenderTargetView(g_pipeline.output_texture.Get(),
                                                   &output_rtv_desc,
                                                   g_pipeline.output_srgb_rtv.GetAddressOf()))) {
            return fail_with_backoff();
        }
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
        return fail_with_backoff();
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
        return fail_with_backoff();
    }

    D3D11_TEXTURE2D_DESC stencil_desc{};
    stencil_desc.Width = width;
    stencil_desc.Height = height;
    stencil_desc.MipLevels = 1;
    stencil_desc.ArraySize = 1;
    stencil_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    stencil_desc.SampleDesc.Count = 1;
    stencil_desc.Usage = D3D11_USAGE_DEFAULT;
    stencil_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(
            &stencil_desc, nullptr, g_pipeline.stencil_texture.GetAddressOf())) ||
        FAILED(device->CreateDepthStencilView(
            g_pipeline.stencil_texture.Get(), nullptr, g_pipeline.stencil_dsv.GetAddressOf()))) {
        return fail_with_backoff();
    }

    g_pipeline.width = width;
    g_pipeline.height = height;
    g_pipeline.backbuffer_format = bb_desc.Format;
    g_sized_resources_ready.store(true);
    g_sized_retry_after_tick.store(0, std::memory_order_relaxed);
    g_width.store(width);
    g_height.store(height);
    log::InfoF("smaa_resources_ready width=%u height=%u format=%u output=%s stencil=1",
               width,
               height,
               bb_desc.Format,
               g_pipeline.direct_output_srgb_rtv ? "direct_srgb" : "srgb_copy_fallback");
    return true;
}

template <typename Shader>
struct ShaderClassState {
    // D3D11 exposes a bounded class-instance array. Keeping ownership inline
    // avoids a heap allocation on every Present (and makes bad_alloc unable to
    // escape a graphics hook).
    std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> instances{};
    UINT count = 0;
};

template <typename Shader, typename Getter>
void CaptureShaderState(Getter&& getter,
                        ComPtr<Shader>& shader,
                        ShaderClassState<Shader>& class_state) noexcept {
    std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> raw_instances{};
    UINT count = static_cast<UINT>(raw_instances.size());
    getter(shader.GetAddressOf(), raw_instances.data(), &count);
    class_state.count = std::min(count, static_cast<UINT>(raw_instances.size()));
    for (UINT index = 0; index < class_state.count; ++index) {
        if (raw_instances[index] != nullptr) {
            class_state.instances[index].Attach(raw_instances[index]);
        }
        // Preserve null slots and the original slot count. Compressing the
        // list shifts dynamic-linkage interface indices when a shader has a
        // sparse class-instance array.
    }
}

template <typename Shader, typename Setter>
void RestoreShaderState(Setter&& setter,
                        const ComPtr<Shader>& shader,
                        const ShaderClassState<Shader>& class_state) noexcept {
    std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> raw_instances{};
    const UINT count = std::min(class_state.count, static_cast<UINT>(raw_instances.size()));
    for (UINT index = 0; index < count; ++index) {
        raw_instances[index] = class_state.instances[index].Get();
    }
    setter(shader.Get(), count == 0 ? nullptr : raw_instances.data(), count);
}

struct ScopedPipelineState {
    ID3D11DeviceContext* context = nullptr;
    bool capture_complete = false;
    bool restored = false;
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
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicate_value = FALSE;
    ComPtr<ID3D11InputLayout> input_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ShaderClassState<ID3D11VertexShader> vs_class_instances;
    ShaderClassState<ID3D11PixelShader> ps_class_instances;
    ShaderClassState<ID3D11GeometryShader> gs_class_instances;
    ShaderClassState<ID3D11HullShader> hs_class_instances;
    ShaderClassState<ID3D11DomainShader> ds_class_instances;
    std::array<ComPtr<ID3D11Buffer>, 1> vs_cbs{};
    std::array<ComPtr<ID3D11Buffer>, 1> ps_cbs{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 8> ps_srvs{};
    // Only s0 is mutated; save and restore exactly that slot.
    std::array<ComPtr<ID3D11SamplerState>, 1> ps_samplers{};

    explicit ScopedPipelineState(ID3D11DeviceContext* ctx) noexcept : context(ctx) {}

    void Capture() noexcept {
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reinterpret_cast<ID3D11RenderTargetView**>(rtvs.data()), dsv.GetAddressOf());
        context->RSGetViewports(&viewport_count, viewports);
        context->OMGetBlendState(blend_state.GetAddressOf(), blend_factor, &sample_mask);
        context->OMGetDepthStencilState(depth_state.GetAddressOf(), &stencil_ref);
        context->RSGetState(rasterizer_state.GetAddressOf());
        context->GetPredication(predicate.GetAddressOf(), &predicate_value);
        context->IAGetInputLayout(input_layout.GetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        CaptureShaderState(
            [&](ID3D11VertexShader** shader, ID3D11ClassInstance** instances, UINT* count) {
                context->VSGetShader(shader, instances, count);
            },
            vs,
            vs_class_instances);
        CaptureShaderState(
            [&](ID3D11PixelShader** shader, ID3D11ClassInstance** instances, UINT* count) {
                context->PSGetShader(shader, instances, count);
            },
            ps,
            ps_class_instances);
        CaptureShaderState(
            [&](ID3D11GeometryShader** shader, ID3D11ClassInstance** instances, UINT* count) {
                context->GSGetShader(shader, instances, count);
            },
            gs,
            gs_class_instances);
        CaptureShaderState(
            [&](ID3D11HullShader** shader, ID3D11ClassInstance** instances, UINT* count) {
                context->HSGetShader(shader, instances, count);
            },
            hs,
            hs_class_instances);
        CaptureShaderState(
            [&](ID3D11DomainShader** shader, ID3D11ClassInstance** instances, UINT* count) {
                context->DSGetShader(shader, instances, count);
            },
            ds,
            ds_class_instances);
        context->VSGetConstantBuffers(0, 1, reinterpret_cast<ID3D11Buffer**>(vs_cbs.data()));
        context->PSGetConstantBuffers(0, 1, reinterpret_cast<ID3D11Buffer**>(ps_cbs.data()));
        context->PSGetShaderResources(0, 8, reinterpret_cast<ID3D11ShaderResourceView**>(ps_srvs.data()));
        context->PSGetSamplers(0, static_cast<UINT>(ps_samplers.size()),
                               reinterpret_cast<ID3D11SamplerState**>(ps_samplers.data()));
        capture_complete = true;
    }

    void Restore() noexcept {
        if (restored || context == nullptr) {
            return;
        }
        // Capture uses getters only. If a structured fault interrupted it,
        // no SMAA state was bound and replaying a partial/default snapshot
        // would itself corrupt the game's pipeline.
        if (!capture_complete) {
            restored = true;
            return;
        }
        // Every setter below is idempotent.  Publish completion only after the
        // entire stock pipeline has been rebound so an SEH interruption can
        // retry from the beginning instead of silently skipping the remaining
        // state and leaking SMAA bindings into the next game frame.
        context->OMSetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
            reinterpret_cast<ID3D11RenderTargetView* const*>(rtvs.data()),
            dsv.Get());
        context->RSSetViewports(viewport_count, viewports);
        context->OMSetBlendState(blend_state.Get(), blend_factor, sample_mask);
        context->OMSetDepthStencilState(depth_state.Get(), stencil_ref);
        context->RSSetState(rasterizer_state.Get());
        context->SetPredication(predicate.Get(), predicate_value);
        context->IASetInputLayout(input_layout.Get());
        context->IASetPrimitiveTopology(topology);
        RestoreShaderState(
            [&](ID3D11VertexShader* shader,
                ID3D11ClassInstance* const* instances,
                UINT count) { context->VSSetShader(shader, instances, count); },
            vs,
            vs_class_instances);
        RestoreShaderState(
            [&](ID3D11PixelShader* shader,
                ID3D11ClassInstance* const* instances,
                UINT count) { context->PSSetShader(shader, instances, count); },
            ps,
            ps_class_instances);
        RestoreShaderState(
            [&](ID3D11GeometryShader* shader,
                ID3D11ClassInstance* const* instances,
                UINT count) { context->GSSetShader(shader, instances, count); },
            gs,
            gs_class_instances);
        RestoreShaderState(
            [&](ID3D11HullShader* shader,
                ID3D11ClassInstance* const* instances,
                UINT count) { context->HSSetShader(shader, instances, count); },
            hs,
            hs_class_instances);
        RestoreShaderState(
            [&](ID3D11DomainShader* shader,
                ID3D11ClassInstance* const* instances,
                UINT count) { context->DSSetShader(shader, instances, count); },
            ds,
            ds_class_instances);
        ID3D11Buffer* vs_cb = vs_cbs[0].Get();
        ID3D11Buffer* ps_cb = ps_cbs[0].Get();
        context->VSSetConstantBuffers(0, 1, &vs_cb);
        context->PSSetConstantBuffers(0, 1, &ps_cb);
        context->PSSetShaderResources(
            0, static_cast<UINT>(ps_srvs.size()), reinterpret_cast<ID3D11ShaderResourceView* const*>(ps_srvs.data()));
        context->PSSetSamplers(0, static_cast<UINT>(ps_samplers.size()),
                               reinterpret_cast<ID3D11SamplerState* const*>(ps_samplers.data()));
        restored = true;
    }

    [[nodiscard]] bool IsRestored() const noexcept { return restored; }

    void Abandon() noexcept {
        // Used only after all SEH-protected restore attempts fault.  Prevent
        // the destructor from issuing an unguarded COM call through a stale
        // context; stock AA is already marked available for the next frame.
        context = nullptr;
        restored = true;
    }

    // CleanupActiveSmaaPass performs the SEH-guarded restore explicitly. The
    // destructor must not issue an unguarded COM call during error recovery.
    ~ScopedPipelineState() noexcept = default;
};
static_assert(std::is_nothrow_constructible_v<ScopedPipelineState, ID3D11DeviceContext*>);

alignas(ScopedPipelineState) thread_local std::array<unsigned char, sizeof(ScopedPipelineState)>
    g_pipeline_state_storage{};
thread_local bool g_pipeline_state_storage_usable = true;

void SafeReleaseUnknown(IUnknown* object) noexcept {
    if (object == nullptr) {
        return;
    }
    __try {
        object->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Restore an exact pre-SMAA snapshot without depending on the global resource
// graph. This is also usable while that graph is being discarded after a
// state-restore fault.
bool RestoreTextureToBackbuffer(ID3D11DeviceContext* context,
                                ID3D11Texture2D* backbuffer,
                                ID3D11Texture2D* source) noexcept {
    if (context == nullptr || backbuffer == nullptr || source == nullptr) {
        return false;
    }

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Predicate* predicate = nullptr;
    BOOL predicate_value = FALSE;
    bool targets_captured = false;
    bool targets_unbound = false;
    bool targets_restored = false;
    bool predicate_captured = false;
    bool predicate_disabled = false;
    bool predicate_restored = false;
    bool copy_issued = false;

    __try {
        __try {
            context->OMGetRenderTargets(
                static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
            targets_captured = true;
            context->GetPredication(&predicate, &predicate_value);
            predicate_captured = true;
            context->SetPredication(nullptr, FALSE);
            predicate_disabled = true;
            context->OMSetRenderTargets(0, nullptr, nullptr);
            targets_unbound = true;
            context->CopyResource(backbuffer, source);
            copy_issued = true;
            context->OMSetRenderTargets(
                static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
            targets_restored = true;
            context->SetPredication(predicate, predicate_value);
            predicate_restored = true;
        } __finally {
            if (targets_captured && targets_unbound && !targets_restored) {
                __try {
                    context->OMSetRenderTargets(
                        static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
                    targets_restored = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            if (predicate_captured && predicate_disabled && !predicate_restored) {
                __try {
                    context->SetPredication(predicate, predicate_value);
                    predicate_restored = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            for (ID3D11RenderTargetView* rtv : rtvs) {
                SafeReleaseUnknown(rtv);
            }
            SafeReleaseUnknown(dsv);
            SafeReleaseUnknown(predicate);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return copy_issued && targets_restored && predicate_restored;
}

bool CleanupActiveSmaaPass() noexcept {
    ActiveSmaaPass* const pass = g_active_smaa_pass;
    if (pass == nullptr) {
        return true;
    }
    bool cleanup_ok = !pass->resource_graph_uncertain;

    if (pass->constant_buffer_mapped && pass->mapped_context != nullptr) {
        __try {
            pass->mapped_context->Unmap(g_pipeline.constant_buffer.Get(), 0);
            pass->constant_buffer_mapped = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cleanup_ok = false;
            MarkReplacementUnavailable();
            pass->resource_graph_uncertain = true;
        }
    }
    __try {
        pass->mapped_context.Reset();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cleanup_ok = false;
        (void)pass->mapped_context.Detach();
        pass->resource_graph_uncertain = true;
        MarkReplacementUnavailable();
    }

    if (pass->pipeline_state != nullptr) {
        // Restoration itself talks to a potentially stale D3D context.  A
        // second SEH boundary makes cleanup best-effort while still ensuring
        // that the mutex is released below. Retry once from the beginning:
        // setters are idempotent and Restore publishes its latch only after
        // every binding has completed.
        for (unsigned int attempt = 0;
             attempt < 2 && !pass->pipeline_state->IsRestored();
             ++attempt) {
            __try {
                pass->pipeline_state->Restore();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!pass->pipeline_state->IsRestored()) {
            cleanup_ok = false;
            pass->pipeline_state->Abandon();
            pass->resource_graph_uncertain = true;
        }
        ScopedPipelineState* const pipeline_state = pass->pipeline_state;
        pass->pipeline_state = nullptr;
        __try {
            pipeline_state->~ScopedPipelineState();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cleanup_ok = false;
            g_pipeline_state_storage_usable = false;
            pass->resource_graph_uncertain = true;
            MarkReplacementUnavailable();
        }
    }

    if (!cleanup_ok && pass->source_snapshot_valid &&
        pass->backbuffer_write_started) {
        if (!RestoreTextureToBackbuffer(pass->context.Get(),
                                        pass->backbuffer.Get(),
                                        pass->source_snapshot.Get())) {
            log::Error("smaa_failed_pass_source_restore_fail");
        } else {
            log::Info("smaa_failed_pass_source_restored");
        }
    }

    if (pass->resource_graph_uncertain && pass->mutex_locked) {
        __try {
            ResetPipelineResourcesLocked(nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cleanup_ok = false;
            MarkReplacementUnavailable();
        }
    }

    if (pass->mutex_locked && pass->mutex != nullptr) {
        pass->mutex->unlock();
        pass->mutex_locked = false;
    }
    g_active_smaa_pass = nullptr;
    __try {
        pass->~ActiveSmaaPass();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cleanup_ok = false;
        g_active_smaa_pass_storage_usable = false;
        // A broken third-party COM implementation can fault from Release.
        // The process owns that retained reference until exit. Poison the TLS
        // arena so it is never reconstructed over a partially destroyed object;
        // the optional SMAA path remains fail-open instead of crashing the game.
        MarkReplacementUnavailable();
    }
    if (!cleanup_ok) {
        MarkReplacementUnavailable();
    }
    return cleanup_ok;
}

bool UpdateConstants(ID3D11DeviceContext* context, UINT width, UINT height) {
    ActiveSmaaPass* const active_pass = g_active_smaa_pass;
    if (active_pass == nullptr || context == nullptr) {
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            g_pipeline.constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    active_pass->mapped_context = context;
    active_pass->constant_buffer_mapped = true;
    if (mapped.pData == nullptr) {
        context->Unmap(g_pipeline.constant_buffer.Get(), 0);
        active_pass->constant_buffer_mapped = false;
        active_pass->mapped_context.Reset();
        return false;
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
    active_pass->constant_buffer_mapped = false;
    active_pass->mapped_context.Reset();
    return true;
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
    // Fullscreen SMAA uses only VS/PS. Inheriting the game's geometry or
    // tessellation stages can transform, reject, or invalidate the triangle.
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* const cb = g_pipeline.constant_buffer.Get();
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetConstantBuffers(0, 1, &cb);

}

bool ApplySmaa(IDXGISwapChain* swapchain,
               ID3D11Device*& device_identity,
               unsigned long long& resource_generation) {
    device_identity = nullptr;
    resource_generation = 0;
    // Present re-entry is normally blocked by the detour, but keep this
    // invariant explicit so a future caller cannot overwrite the recovery
    // record belonging to an already-active pass.
    if (g_active_smaa_pass != nullptr || !g_active_smaa_pass_storage_usable ||
        !g_pipeline_state_storage_usable) {
        return false;
    }
    ActiveSmaaPass* const active_pass =
        ::new (static_cast<void*>(g_active_smaa_pass_storage.data())) ActiveSmaaPass{};
    g_active_smaa_pass = active_pass;
    active_pass->device.Attach(RetainSlotValue<ID3D11Device>(g_device_slot));
    active_pass->context.Attach(RetainSlotValue<ID3D11DeviceContext>(g_context_slot));
    if (active_pass->device == nullptr || active_pass->context == nullptr) {
        (void)CleanupActiveSmaaPass();
        return false;
    }

    active_pass->context->GetDevice(active_pass->context_device.GetAddressOf());
    if (FAILED(swapchain->GetDevice(
            IID_PPV_ARGS(active_pass->swapchain_device.GetAddressOf()))) ||
        active_pass->context_device.Get() != active_pass->device.Get() ||
        active_pass->swapchain_device.Get() != active_pass->device.Get()) {
        (void)CleanupActiveSmaaPass();
        return false;
    }

    active_pass->mutex = &g_mutex;
    g_mutex.lock();
    active_pass->mutex_locked = true;

    if (!EnsureStaticResources(active_pass->device.Get()) ||
        !EnsureSizedResources(active_pass->device.Get(), swapchain)) {
        (void)CleanupActiveSmaaPass();
        return false;
    }
    resource_generation = g_resource_generation.load(std::memory_order_acquire);

    D3D11_TEXTURE2D_DESC bb_desc{};
    active_pass->backbuffer->GetDesc(&bb_desc);

    active_pass->pipeline_state = ::new (static_cast<void*>(g_pipeline_state_storage.data()))
        ScopedPipelineState(active_pass->context.Get());
    active_pass->pipeline_state->Capture();

    // Predication applies to copies, resolves, clears, and draws as well as
    // ordinary rendering. A false predicate inherited from the game would
    // otherwise suppress the entire replacement while the CPU reports a
    // successful pass and stock AA remains disabled.
    active_pass->context->SetPredication(nullptr, FALSE);

    // The game commonly leaves its present target bound. Capture that state,
    // unbind it for the copies, and restore it only after all three SMAA passes
    // complete. This keeps the D3D11 read/write hazard rules explicit.
    active_pass->context->OMSetRenderTargets(0, nullptr, nullptr);

    // ResolveSubresource requires a concrete member of the typeless family.
    const DXGI_FORMAT resolve_format = SmaaLinearFormat(bb_desc.Format);

    if (bb_desc.SampleDesc.Count > 1) {
        active_pass->context->CopyResource(
            g_pipeline.multisample_source_texture.Get(), active_pass->backbuffer.Get());
        active_pass->context->ResolveSubresource(
            g_pipeline.source_texture.Get(), 0, active_pass->backbuffer.Get(), 0, resolve_format);
    } else {
        active_pass->context->CopyResource(
            g_pipeline.source_texture.Get(), active_pass->backbuffer.Get());
    }
    active_pass->source_snapshot = g_pipeline.multisample_source_texture
        ? g_pipeline.multisample_source_texture
        : g_pipeline.source_texture;
    active_pass->source_snapshot_valid = active_pass->source_snapshot != nullptr;

    if (!UpdateConstants(active_pass->context.Get(), bb_desc.Width, bb_desc.Height)) {
        (void)CleanupActiveSmaaPass();
        return false;
    }
    ID3D11DeviceContext* const context = active_pass->context.Get();
    PrepareCommonState(context, bb_desc.Width, bb_desc.Height);

    const float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(g_pipeline.edges_rtv.Get(), clear_rgba);
    context->ClearRenderTargetView(g_pipeline.blend_rtv.Get(), clear_rgba);
    context->ClearDepthStencilView(
        g_pipeline.stencil_dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Pass 1 – Edge detection. Only colorTex (t0) is needed; the former t1
    // (colorTexGamma) slot has been removed as it was never read by EdgePS.
    {
        context->VSSetShader(g_pipeline.edge_vs.Get(), nullptr, 0);
        context->PSSetShader(g_pipeline.edge_ps.Get(), nullptr, 0);
        ID3D11SamplerState* const sampler = g_pipeline.point_sampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        context->OMSetDepthStencilState(g_pipeline.edge_stencil_state.Get(), 1);
        ID3D11RenderTargetView* const rtv = g_pipeline.edges_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, g_pipeline.stencil_dsv.Get());
        ID3D11ShaderResourceView* srvs[8] = {
            g_pipeline.source_linear_srv.Get(), nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr};
        context->PSSetShaderResources(0, 8, srvs);
        context->Draw(3, 0);
    }

    // Pass 2 – Blend weight calculation. edgesTex=t4, areaTex=t6, searchTex=t7.
    {
        context->VSSetShader(g_pipeline.blend_vs.Get(), nullptr, 0);
        context->PSSetShader(g_pipeline.blend_ps.Get(), nullptr, 0);
        ID3D11SamplerState* const sampler = g_pipeline.linear_sampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        context->OMSetDepthStencilState(g_pipeline.weight_stencil_state.Get(), 1);
        ID3D11RenderTargetView* const rtv = g_pipeline.blend_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, g_pipeline.stencil_dsv.Get());
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
        ID3D11SamplerState* const sampler = g_pipeline.linear_sampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        context->OMSetDepthStencilState(g_pipeline.depth_state.Get(), 0);
        ID3D11RenderTargetView* const rtv = g_pipeline.direct_output_srgb_rtv
            ? g_pipeline.direct_output_srgb_rtv.Get()
            : g_pipeline.output_srgb_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        active_pass->backbuffer_write_started =
            g_pipeline.direct_output_srgb_rtv != nullptr;
        ID3D11ShaderResourceView* srvs[8] = {
            g_pipeline.source_srgb_srv.Get(), nullptr, nullptr, nullptr,
            nullptr, g_pipeline.blend_srv.Get(), nullptr, nullptr};
        context->PSSetShaderResources(0, 8, srvs);
        context->Draw(3, 0);
    }

    ID3D11ShaderResourceView* null_srvs[8] = {};
    context->PSSetShaderResources(0, 8, null_srvs);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (!g_pipeline.direct_output_srgb_rtv) {
        active_pass->backbuffer_write_started = true;
        context->CopyResource(active_pass->backbuffer.Get(), g_pipeline.output_texture.Get());
    }

    device_identity = active_pass->device.Get();
    return CleanupActiveSmaaPass();
}

// g_mutex is held by the caller. This function intentionally uses raw COM
// pointers only: a structured exception from a stale overlay/device object must
// not bypass C++ destructors while running inside the Present detour.
bool RestoreSmaaSourceLocked(IDXGISwapChain* swapchain,
                             ID3D11Device* expected_device,
                             unsigned long long expected_generation) noexcept {
    ID3D11DeviceContext* context = nullptr;
    bool restored = false;

    __try {
        __try {
            if (swapchain == nullptr || expected_device == nullptr ||
                g_resource_generation.load(std::memory_order_acquire) !=
                    expected_generation ||
                g_pipeline.owner_device.Get() != expected_device ||
                g_pipeline.owner_swapchain.Get() != swapchain ||
                g_pipeline.output_backbuffer == nullptr ||
                g_pipeline.source_texture == nullptr) {
                __leave;
            }

            ID3D11Texture2D* const source = g_pipeline.multisample_source_texture
                ? g_pipeline.multisample_source_texture.Get()
                : g_pipeline.source_texture.Get();
            expected_device->GetImmediateContext(&context);
            if (context == nullptr || source == nullptr) {
                __leave;
            }
            restored = RestoreTextureToBackbuffer(
                context, g_pipeline.output_backbuffer.Get(), source);
        } __finally {
            SafeReleaseUnknown(context);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return restored;
}

bool ResetPipelineAfterRestoreFailureLocked() noexcept {
    __try {
        ResetPipelineResourcesLocked(nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkReplacementUnavailable();
        return false;
    }
}

bool RestoreSmaaSourceAfterRejectedPresent(
    IDXGISwapChain* swapchain,
    ID3D11Device* expected_device,
    unsigned long long expected_generation) noexcept {
    try {
        g_mutex.lock();
    } catch (...) {
        MarkReplacementUnavailable();
        return false;
    }

    const bool restored = RestoreSmaaSourceLocked(
        swapchain, expected_device, expected_generation);
    if (!restored) {
        // The current graph can no longer prove whether its backbuffer contains
        // source or filtered color. Drop it so stock AA remains authoritative.
        (void)ResetPipelineAfterRestoreFailureLocked();
        log::Error("smaa_failed_present_restore_fail");
    } else {
        log::Info("smaa_failed_present_source_restored");
    }
    g_mutex.unlock();
    return restored;
}

bool BeginResizePreparation(IDXGISwapChain* swapchain) noexcept {
    if (!g_hook_install_allowed.load(std::memory_order_acquire) ||
        !IsOurSwapChain(swapchain)) {
        return false;
    }
    IncrementDiagnostic(g_resize_count);
    ClearRejectedTargetPair();
    MarkHookDiscoveryNeeded();
    try {
        g_mutex.lock();
        return true;
    } catch (...) {
        MarkReplacementUnavailable();
        return false;
    }
}

// The caller owns g_mutex. This is deliberately separate from the lock
// acquisition so the ResizeBuffers detour can release the mutex from an SEH
// finally block if a stale overlay/device COM object faults during reset.
void PrepareResizeBuffersLocked(IDXGISwapChain* swapchain) {
    ResetSizedResourcesLocked();
    g_pipeline.owner_swapchain = swapchain;
}

// Keep C++ exception handling out of the SEH detour below.  MSVC rejects a
// function that mixes __try/__finally with a C++ try/catch, while D3D/overlay
// faults still need the outer SEH boundary.  This small guard handles only
// allocation/COM-wrapper exceptions and leaves structured faults to the
// caller's fail-open handler.
bool ApplySmaaWithCppGuard(IDXGISwapChain* swapchain,
                           ID3D11Device*& device_identity,
                           unsigned long long& resource_generation) noexcept {
    try {
        return ApplySmaa(swapchain, device_identity, resource_generation);
    } catch (...) {
        device_identity = nullptr;
        resource_generation = 0;
        if (g_active_smaa_pass != nullptr) {
            g_active_smaa_pass->resource_graph_uncertain = true;
        }
        (void)CleanupActiveSmaaPass();
        MarkReplacementUnavailable();
        return false;
    }
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags) {
    const bool active = EnterHookCall(g_present_active_calls);
    const PresentFn original = g_present_original;
    if (!active) {
        return E_FAIL;
    }
    if (original == nullptr) {
        LeaveHookCall(g_present_active_calls);
        return E_FAIL;
    }
    IncrementDiagnostic(g_present_count);
    HRESULT result = E_FAIL;
    bool owns_reentry = false;
    bool apply_attempted = false;
    bool pass_completed = false;
    ID3D11Device* applied_device = nullptr;
    unsigned long long applied_generation = 0;
    bool our_swapchain = false;
    __try {
        if (!g_present_reentry) {
            owns_reentry = true;
            g_present_reentry = true;
            our_swapchain = IsOurSwapChain(swapchain);
            if (g_enabled.load(std::memory_order_acquire) &&
                g_hook_install_allowed.load(std::memory_order_acquire) &&
                HasCompleteHookPair() && ValidateOwnedHookBytes() && our_swapchain &&
                ShouldRunSmaaPresentPass(flags)) {
                // Publish failure before entering D3D. If a device fault or
                // structured exception interrupts the pass, stock AA resumes
                // on the next frame instead of remaining disabled forever.
                MarkReplacementUnavailable();
                apply_attempted = true;
                __try {
                    pass_completed = ApplySmaaWithCppGuard(
                        swapchain, applied_device, applied_generation);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // A third-party overlay or a device-reset race can make a
                    // COM target disappear between the guarded discovery and
                    // the draw. Treat that as a failed replacement frame;
                    // never let an optional post-process crash the game.
                    if (g_active_smaa_pass != nullptr) {
                        g_active_smaa_pass->resource_graph_uncertain = true;
                    }
                    (void)CleanupActiveSmaaPass();
                    MarkReplacementUnavailable();
                }
            } else if (our_swapchain && (flags & DXGI_PRESENT_TEST) == 0) {
                // TEST calls do not represent a frame and must not invalidate a
                // previously proven generation. Other skipped real presents,
                // including DO_NOT_WAIT, fail open for the next frame.
                MarkReplacementUnavailable();
            }
        }
        result = original(swapchain, sync_interval, flags);
        if (apply_attempted && pass_completed &&
            ShouldRestoreSmaaSource(flags, result)) {
            // The real Present rejected this frame. Put the pre-SMAA image
            // back before returning so an application retry cannot filter an
            // already-filtered backbuffer or display a partially owned frame.
            pass_completed = RestoreSmaaSourceAfterRejectedPresent(
                swapchain, applied_device, applied_generation) && pass_completed;
        }
        if (!apply_attempted && our_swapchain &&
            ShouldInvalidateAfterSkippedPresent(flags, result)) {
            MarkReplacementUnavailable();
        }
        if (apply_attempted) {
            if (ShouldPublishSmaaSuccess(pass_completed, result) &&
                PublishReplacementSuccessIfCurrent(
                    swapchain, applied_device, applied_generation)) {
                IncrementDiagnostic(g_apply_count);
            } else {
                MarkReplacementUnavailable();
                IncrementDiagnostic(g_fail_count);
            }
        }
    } __finally {
        if (owns_reentry) {
            g_present_reentry = false;
        }
        LeaveHookCall(g_present_active_calls);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* swapchain,
                                              UINT buffer_count,
                                              UINT width,
                                              UINT height,
                                              DXGI_FORMAT new_format,
                                              UINT swap_chain_flags) {
    const bool active = EnterHookCall(g_resize_active_calls);
    const ResizeBuffersFn original = g_resize_buffers_original;
    if (!active) {
        return E_FAIL;
    }
    if (original == nullptr) {
        LeaveHookCall(g_resize_active_calls);
        return E_FAIL;
    }

    HRESULT result = E_FAIL;
    bool resize_mutex_locked = false;
    __try {
        __try {
            __try {
                resize_mutex_locked = BeginResizePreparation(swapchain);
                if (resize_mutex_locked) {
                    PrepareResizeBuffersLocked(swapchain);
                }
            } __finally {
                if (resize_mutex_locked) {
                    g_mutex.unlock();
                    resize_mutex_locked = false;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            MarkReplacementUnavailable();
        }
        result = original(swapchain, buffer_count, width, height, new_format, swap_chain_flags);
    } __finally {
        LeaveHookCall(g_resize_active_calls);
    }
    return result;
}

}  // namespace

bool Initialize(std::uintptr_t module_base,
                std::uintptr_t device_slot,
                std::uintptr_t context_slot,
                std::uintptr_t swapchain_slot,
                std::uintptr_t present_rtv_slot) {
    (void)module_base;
    (void)present_rtv_slot;
    // Serialize reinitialization with a previous teardown.  This is normally
    // a one-shot startup path, but keeping the lifecycle boundary explicit
    // prevents a stale game detour from racing a retry and installing against
    // half-published slots.
    g_hook_install_allowed.store(false, std::memory_order_release);
    MarkReplacementUnavailable();
    std::lock_guard<std::mutex> hook_lock(g_hook_mutex);
    g_device_slot = device_slot;
    g_context_slot = context_slot;
    g_swapchain_slot = swapchain_slot;
    g_hook_retry_after_tick.store(0, std::memory_order_relaxed);
    g_present_hook_bytes_captured.store(false, std::memory_order_relaxed);
    g_resize_hook_bytes_captured.store(false, std::memory_order_relaxed);
    g_owned_hook_bytes_valid.store(false, std::memory_order_release);
    g_present_vtable_target.store(nullptr, std::memory_order_relaxed);
    g_resize_vtable_target.store(nullptr, std::memory_order_relaxed);
    g_present_redirect_chain.Clear();
    g_resize_redirect_chain.Clear();
    g_present_hook_bytes.Clear();
    g_resize_buffers_hook_bytes.Clear();
    MarkHookDiscoveryNeeded();
    ClearRejectedTargetPair();
    {
        std::lock_guard<std::mutex> resource_lock(g_mutex);
        if (!PrepareShaderBytecodeLocked()) {
            log::Error("smaa_initialize_fail reason=shader_precompile");
            return false;
        }
    }
    return true;
}

void Activate() {
    MarkReplacementUnavailable();
    MarkHookDiscoveryNeeded();
    g_hook_install_allowed.store(true, std::memory_order_release);
}

bool Shutdown() {
    // Parent game detours can remain callable for the rest of the process.
    // Close discovery first, then wait for any active SMAA calls before
    // releasing D3D resources.  We intentionally do not call MH_RemoveHook:
    // this MinHook fork frees the original trampoline there, and a detour may
    // have already loaded that pointer on another thread.
    g_hook_install_allowed.store(false, std::memory_order_release);
    MarkReplacementUnavailable();
    g_hook_discovery_needed.store(false, std::memory_order_release);
    g_hook_next_probe_tick.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> hook_lock(g_hook_mutex);
        HookTransitionGuard transition;
        if (!transition.active()) {
            log::Error("smaa_shutdown_deferred hook_transition_busy");
            return false;
        }
        (void)RetainOwnedHookTarget(g_present_hook_target, "present");
        (void)RetainOwnedHookTarget(g_resize_buffers_hook_target, "resize");
        RefreshHookOwnershipState();
        g_owned_hook_bytes_valid.store(false, std::memory_order_release);
        g_present_hook_bytes_captured.store(false, std::memory_order_relaxed);
        g_resize_hook_bytes_captured.store(false, std::memory_order_relaxed);
        g_present_hook_bytes.Clear();
        g_resize_buffers_hook_bytes.Clear();
        g_hook_retry_after_tick.store(0, std::memory_order_relaxed);
        ClearRejectedTargetPair();
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ResetPipelineResourcesLocked(nullptr);
    }
    return true;
}

void MaybeInstallHooks() {
    if (!g_hook_install_allowed.load(std::memory_order_acquire)) {
        MarkReplacementUnavailable();
        return;
    }

    // This function is reached from render-path detours.  Once both DXGI
    // hooks have been validated/owned, avoid taking a COM reference and
    // remapping the vtable on every frame.  Discovery is reopened by
    // Initialize/PrepareResizeBuffers, while the one-second probe still
    // catches an overlay that replaces the vtable after startup.  A partial
    // first install is always allowed through so the missing half can be
    // completed.
    const unsigned long long discovery_now = GetTickCount64();
    const bool partial_ownership = HasPartialHookOwnership();
    const bool hooks_owned = g_any_hook_retained.load(std::memory_order_acquire);
    const unsigned long long next_probe =
        g_hook_next_probe_tick.load(std::memory_order_relaxed);
    if (!g_hook_discovery_needed.load(std::memory_order_acquire) && hooks_owned &&
        !partial_ownership && next_probe != 0 && discovery_now < next_probe) {
        return;
    }

    const unsigned long long retry_after =
        g_hook_retry_after_tick.load(std::memory_order_relaxed);
    // A partial install is the common failure mode when an overlay exposes
    // only one compatible DXGI slot.  It must not bypass the retry backoff:
    // otherwise every Present still performs AddRef/vtable/PE work before the
    // locked section rejects the unchanged pair.  A zero deadline deliberately
    // permits the initial completion attempt.
    if (retry_after != 0 && discovery_now < retry_after) {
        return;
    }

    // Hold the COM object for the complete discovery/validation/create
    // transaction. A raw slot read alone lets a resize/device teardown free
    // the vtable between the SEH-protected read and MH_CreateHook.
    ComPtr<IDXGISwapChain> swapchain_owner;
    swapchain_owner.Attach(RetainSlotValue<IDXGISwapChain>(g_swapchain_slot));
    IDXGISwapChain* const swapchain = swapchain_owner.Get();
    if (swapchain == nullptr) {
        MarkReplacementUnavailable();
        g_hook_retry_after_tick.store(
            discovery_now + 1000, std::memory_order_relaxed);
        return;
    }

    void* present_vtable_target = nullptr;
    void* resize_vtable_target = nullptr;
    if (!ReadSwapChainTargets(swapchain, present_vtable_target, resize_vtable_target)) {
        MarkReplacementUnavailable();
        log::Error("smaa_hook_target_missing");
        g_hook_retry_after_tick.store(GetTickCount64() + 1000, std::memory_order_relaxed);
        return;
    }
    RedirectChainCapture present_resolution =
        ResolveCompatibleHookTarget(present_vtable_target);
    RedirectChainCapture resize_resolution =
        ResolveCompatibleHookTarget(resize_vtable_target);
    void* present_target = present_resolution.hook_target;
    void* resize_buffers_target = resize_resolution.hook_target;
    ForgetRejectedPairIfChanged(swapchain, present_target, resize_buffers_target);
    if (IsRejectedTargetPair(swapchain, present_target, resize_buffers_target) &&
        !HasPartialHookOwnership()) {
        return;
    }

    // Fast path still verifies both method addresses. A recreated swapchain is
    // allowed to expose a different DXGI implementation/vtable; treating the
    // old hook pair as process-global would silently stop SMAA on that object.
    if (g_any_hook_retained.load(std::memory_order_acquire) &&
        g_present_hook_target.load(std::memory_order_acquire) == present_target &&
        g_resize_buffers_hook_target.load(std::memory_order_acquire) ==
            resize_buffers_target) {
        if (g_owned_hook_bytes_valid.load(std::memory_order_acquire)) {
            if (OwnedHookBytesIntact()) {
                MarkHookDiscoveryComplete(discovery_now);
                return;
            }
            // The vtable still points at the same addresses, but another
            // injector has overwritten the MinHook stubs.  Do not keep
            // suppressing stock AA on the strength of stale ownership state.
            g_owned_hook_bytes_valid.store(false, std::memory_order_release);
            MarkReplacementUnavailable();
            g_hook_retry_after_tick.store(
                discovery_now + kRejectedTargetRetryMilliseconds,
                std::memory_order_relaxed);
            return;
        }
    }

    std::lock_guard<std::mutex> hook_lock(g_hook_mutex);
    if (!g_hook_install_allowed.load(std::memory_order_acquire)) {
        MarkReplacementUnavailable();
        return;
    }

    // Re-read under the hook-state lock; the engine may have replaced the slot
    // between the lock-free fast path and this serialized transition.
    ComPtr<IDXGISwapChain> locked_swapchain_owner;
    locked_swapchain_owner.Attach(RetainSlotValue<IDXGISwapChain>(g_swapchain_slot));
    IDXGISwapChain* const locked_swapchain = locked_swapchain_owner.Get();
    if (locked_swapchain == nullptr) {
        MarkReplacementUnavailable();
        g_hook_retry_after_tick.store(
            discovery_now + 1000, std::memory_order_relaxed);
        return;
    }
    if (!ReadSwapChainTargets(
            locked_swapchain, present_vtable_target, resize_vtable_target)) {
        MarkReplacementUnavailable();
        log::Error("smaa_hook_target_missing");
        g_hook_retry_after_tick.store(GetTickCount64() + 1000, std::memory_order_relaxed);
        return;
    }
    present_resolution = ResolveCompatibleHookTarget(present_vtable_target);
    resize_resolution = ResolveCompatibleHookTarget(resize_vtable_target);
    present_target = present_resolution.hook_target;
    resize_buffers_target = resize_resolution.hook_target;
    ForgetRejectedPairIfChanged(locked_swapchain, present_target, resize_buffers_target);
    if (IsRejectedTargetPair(locked_swapchain, present_target, resize_buffers_target) &&
        !HasPartialHookOwnership()) {
        return;
    }

    if (g_any_hook_retained.load(std::memory_order_acquire) &&
        g_present_hook_target.load(std::memory_order_acquire) == present_target &&
        g_resize_buffers_hook_target.load(std::memory_order_acquire) ==
            resize_buffers_target) {
        if (g_owned_hook_bytes_valid.load(std::memory_order_acquire) &&
            OwnedHookBytesIntact()) {
            MarkHookDiscoveryComplete(discovery_now);
            return;
        }
        g_owned_hook_bytes_valid.store(false, std::memory_order_release);
        MarkReplacementUnavailable();
        g_hook_retry_after_tick.store(
            discovery_now + kRejectedTargetRetryMilliseconds,
            std::memory_order_relaxed);
        return;
    }

    const unsigned long long locked_now = GetTickCount64();
    const unsigned long long locked_retry_after =
        g_hook_retry_after_tick.load(std::memory_order_relaxed);
    if (locked_retry_after != 0 && locked_now < locked_retry_after) {
        return;
    }

    const void* const previous_present_target =
        g_present_hook_target.load(std::memory_order_acquire);
    const void* const previous_resize_target =
        g_resize_buffers_hook_target.load(std::memory_order_acquire);

    const bool retained_snapshot_missing =
        (previous_present_target != nullptr &&
         !g_present_hook_bytes_captured.load(std::memory_order_acquire)) ||
        (previous_resize_target != nullptr &&
         !g_resize_hook_bytes_captured.load(std::memory_order_acquire));
    if (retained_snapshot_missing) {
        // A MinHook target whose post-create bytes could not be captured can
        // never be authenticated later: another injector may have replaced it
        // in the meantime. Keep the retained detour transparent permanently.
        MarkReplacementUnavailable();
        g_owned_hook_bytes_valid.store(false, std::memory_order_release);
        if (!g_rejected_log_emitted.exchange(true, std::memory_order_acq_rel)) {
            log::Error("smaa_hook_snapshot_missing retained target left fail-open");
        }
        g_hook_retry_after_tick.store(
            GetTickCount64() + kRejectedTargetRetryMilliseconds, std::memory_order_relaxed);
        return;
    }

    // MinHook exposes no disable-only operation in the bundled fork, and
    // MH_RemoveHook frees the trampoline. Never retarget a live detour: an
    // overlay/vtable change simply leaves SMAA unavailable and keeps stock AA
    // enabled. A partial first install is safe to complete as long as its
    // already-owned target is unchanged.
    if ((previous_present_target != nullptr && previous_present_target != present_target) ||
        (previous_resize_target != nullptr && previous_resize_target != resize_buffers_target)) {
        MarkReplacementUnavailable();
        UpdateRejectedTargetPair(locked_swapchain, present_target, resize_buffers_target);
        if (!g_rejected_log_emitted.exchange(true, std::memory_order_acq_rel)) {
            log::WarnF("smaa_hook_retarget_rejected old_present=0x%p old_resize=0x%p "
                       "new_present=0x%p new_resize=0x%p swapchain=0x%p",
                       previous_present_target,
                       previous_resize_target,
                       present_target,
                       resize_buffers_target,
                       locked_swapchain);
        }
        g_hook_retry_after_tick.store(
            GetTickCount64() + kRejectedTargetRetryMilliseconds, std::memory_order_relaxed);
        return;
    }
    const bool need_present = previous_present_target == nullptr;
    const bool need_resize = previous_resize_target == nullptr;

    HookTransitionGuard transition;
    if (!transition.active()) {
        MarkReplacementUnavailable();
        log::Warn("smaa_hook_transition_busy");
        g_hook_retry_after_tick.store(GetTickCount64() + 1000, std::memory_order_relaxed);
        return;
    }

    if (need_present) {
        hook_guard::Guard present_target_guard;
        const bool report_rejection =
            g_rejected_retry_after_tick.load(std::memory_order_relaxed) == 0;
        if (!VerifyHookTarget(
                present_target, "present", present_target_guard, report_rejection)) {
            UpdateRejectedTargetPair(locked_swapchain, present_target, resize_buffers_target);
            return;
        }

        const MH_STATUS present_status =
            MH_CreateHook(present_target,
                          reinterpret_cast<void*>(&DetourPresent),
                          reinterpret_cast<void**>(&g_present_original));
        if (present_status != MH_OK) {
            log::ErrorF("smaa_present_hook_create_fail target=0x%p status=%d",
                        present_target,
                        static_cast<int>(present_status));
            g_hook_retry_after_tick.store(
                GetTickCount64() + 1000, std::memory_order_relaxed);
            return;
        }
        g_hooks_retained_process_lifetime.store(true, std::memory_order_release);
        g_present_hook_target.store(present_target, std::memory_order_release);
        if (!CaptureNewHookBytesLocked(present_target,
                                       reinterpret_cast<const void*>(&DetourPresent),
                                       g_present_hook_bytes,
                                       g_present_hook_bytes_captured)) {
            RefreshHookOwnershipState();
            MarkReplacementUnavailable();
            log::ErrorF("smaa_present_hook_snapshot_fail target=0x%p", present_target);
            g_hook_retry_after_tick.store(
                GetTickCount64() + kRejectedTargetRetryMilliseconds,
                std::memory_order_relaxed);
            return;
        }
        g_present_vtable_target.store(present_resolution.vtable_target,
                                      std::memory_order_release);
        PublishRedirectChain(g_present_redirect_chain, present_resolution);
        if (present_resolution.steam_overlay) {
            log::InfoF("smaa_hook_chain name=present provider=steam_overlay entry=0x%p "
                       "relay=0x%p target=0x%p",
                       present_resolution.vtable_target,
                       present_resolution.relay,
                       present_resolution.hook_target);
        }
        RefreshHookOwnershipState();
    }

    if (need_resize) {
        // Verify the second target immediately before its own create. A
        // concurrent overlay can change the vtable between the two checks;
        // leaving a partial Present hook in place is safer than freeing its
        // trampoline and is retried on a later pass.
        hook_guard::Guard resize_target_guard;
        const bool report_rejection =
            g_rejected_retry_after_tick.load(std::memory_order_relaxed) == 0;
        if (!VerifyHookTarget(
                resize_buffers_target, "resize", resize_target_guard, report_rejection)) {
            UpdateRejectedTargetPair(locked_swapchain, present_target, resize_buffers_target);
            g_hook_retry_after_tick.store(
                GetTickCount64() + 1000, std::memory_order_relaxed);
            return;
        }

        const MH_STATUS resize_status =
            MH_CreateHook(resize_buffers_target,
                          reinterpret_cast<void*>(&DetourResizeBuffers),
                          reinterpret_cast<void**>(&g_resize_buffers_original));
        if (resize_status != MH_OK) {
            log::ErrorF("smaa_resize_hook_create_fail target=0x%p status=%d",
                        resize_buffers_target,
                        static_cast<int>(resize_status));
            g_hook_retry_after_tick.store(
                GetTickCount64() + 1000, std::memory_order_relaxed);
            return;
        }

        g_hooks_retained_process_lifetime.store(true, std::memory_order_release);
        g_resize_buffers_hook_target.store(resize_buffers_target, std::memory_order_release);
        if (!CaptureNewHookBytesLocked(resize_buffers_target,
                                       reinterpret_cast<const void*>(&DetourResizeBuffers),
                                       g_resize_buffers_hook_bytes,
                                       g_resize_hook_bytes_captured)) {
            RefreshHookOwnershipState();
            MarkReplacementUnavailable();
            log::ErrorF("smaa_resize_hook_snapshot_fail target=0x%p", resize_buffers_target);
            g_hook_retry_after_tick.store(
                GetTickCount64() + kRejectedTargetRetryMilliseconds,
                std::memory_order_relaxed);
            return;
        }
        g_resize_vtable_target.store(resize_resolution.vtable_target,
                                     std::memory_order_release);
        PublishRedirectChain(g_resize_redirect_chain, resize_resolution);
        if (resize_resolution.steam_overlay) {
            log::InfoF("smaa_hook_chain name=resize provider=steam_overlay entry=0x%p "
                       "relay=0x%p target=0x%p",
                       resize_resolution.vtable_target,
                       resize_resolution.relay,
                       resize_resolution.hook_target);
        }
    }

    PublishOwnedHookBytesValidityLocked();
    RefreshHookOwnershipState();
    if (!g_owned_hook_bytes_valid.load(std::memory_order_acquire)) {
        MarkReplacementUnavailable();
        g_hook_retry_after_tick.store(
            GetTickCount64() + kRejectedTargetRetryMilliseconds,
            std::memory_order_relaxed);
        return;
    }
    ClearRejectedTargetPair();
    g_hook_retry_after_tick.store(0, std::memory_order_relaxed);
    MarkHookDiscoveryComplete(locked_now);
    log::InfoF("smaa_hooks_%s present=0x%p resize=0x%p swapchain=0x%p",
               (need_present || need_resize) ? "installed" : "retained",
               present_target,
               resize_buffers_target,
               locked_swapchain);
}

bool GetEnabled() {
    return g_enabled.load();
}

bool CanReplaceStockAa() {
    if (!g_hook_install_allowed.load(std::memory_order_acquire)) {
        return false;
    }
    const bool hook_pair_intact = HasCompleteHookPair() && ValidateOwnedHookBytes() &&
                                  CurrentSwapChainTargetsMatchOwnedPath();
    const bool resources_ready = g_resources_ready.load(std::memory_order_acquire) &&
                                  g_sized_resources_ready.load(std::memory_order_acquire);
    const bool current_generation =
        g_successful_pass.load(std::memory_order_acquire) &&
        ReadSlot<IDXGISwapChain>(g_swapchain_slot) ==
            g_successful_swapchain.load(std::memory_order_acquire) &&
        ReadSlot<ID3D11Device>(g_device_slot) ==
            g_successful_device.load(std::memory_order_acquire) &&
        g_successful_generation.load(std::memory_order_acquire) ==
            g_resource_generation.load(std::memory_order_acquire);
    return ShouldSuppressStockAa(g_enabled.load(std::memory_order_acquire),
                                 hook_pair_intact,
                                 hook_pair_intact,
                                 resources_ready,
                                 current_generation);
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    // Re-enabling must prove a pass for the current resource generation before
    // stock AA is disabled again. This trades one double-filtered transition
    // frame for a fail-open path when the device can no longer render SMAA.
    AdvanceResourceGeneration();
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

    AdvanceResourceGeneration();

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
    g_pipeline.edge_stencil_state.Reset();
    g_pipeline.weight_stencil_state.Reset();
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
    g_shader_bytecode = {};
    if (!PrepareShaderBytecodeLocked()) {
        log::ErrorF("smaa_preset_unavailable preset=%d", preset);
    }
}

Stats GetStats() {
    return Stats{
        g_present_count.load(),
        g_apply_count.load(),
        g_fail_count.load(),
        g_resize_count.load(),
        g_width.load(),
        g_height.load(),
        g_any_hook_retained.load(),
        g_resources_ready.load() && g_sized_resources_ready.load(),
        g_enabled.load()};
}

}  // namespace spatch::smaa
