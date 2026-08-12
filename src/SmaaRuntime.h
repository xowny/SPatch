#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>

struct IDXGISwapChain;

namespace spatch::smaa {

struct Stats {
    unsigned long long present_count = 0;
    unsigned long long apply_count = 0;
    unsigned long long fail_count = 0;
    unsigned long long resize_count = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    bool any_hook_retained = false;
    bool resources_ready = false;
    bool enabled = false;
};

// Stock AA may only be suppressed after the replacement has proven that it
// can render.  Keeping this as a pure policy helper makes the fail-open
// contract testable without constructing a D3D11 device in the unit tests.
[[nodiscard]] constexpr bool ShouldSuppressStockAa(bool enabled,
                                                   bool present_hook_installed,
                                                   bool resize_hook_installed,
                                                   bool resources_ready,
                                                   bool successful_pass) noexcept {
    return enabled && present_hook_installed && resize_hook_installed && resources_ready &&
           successful_pass;
}

[[nodiscard]] constexpr bool ShouldRunSmaaPresentPass(unsigned int flags) noexcept {
    // DXGI_PRESENT_TEST probes presentation status without displaying a frame;
    // touching the backbuffer or running three fullscreen passes is invalid.
    // DXGI_PRESENT_DO_NOT_WAIT is still a real Present. It must receive SMAA;
    // if DXGI rejects any real Present, the detour restores the source image
    // before returning so a later retry cannot filter the same frame twice.
    return (flags & DXGI_PRESENT_TEST) == 0;
}

[[nodiscard]] constexpr bool ShouldRestoreSmaaSource(unsigned int flags,
                                                     HRESULT present_result) noexcept {
    // A failed real Present does not consume the frame. Restore the saved
    // pre-SMAA image for every failure, not just the common nonblocking retry,
    // so an application retry cannot filter an already-filtered backbuffer.
    // TEST calls never run the SMAA pass and are excluded defensively.
    return (flags & DXGI_PRESENT_TEST) == 0 && present_result < 0;
}

// pass_completed includes rendering plus restoration of every captured game
// binding and retained COM reference. Publication additionally waits for the
// real Present result so failed/nonblocking presents cannot authorize stock-AA
// suppression on the next frame.
[[nodiscard]] constexpr bool ShouldPublishSmaaSuccess(bool pass_completed,
                                                      HRESULT present_result) noexcept {
    return pass_completed && present_result >= 0;
}

[[nodiscard]] constexpr bool ShouldInvalidateAfterSkippedPresent(
    unsigned int flags, HRESULT present_result) noexcept {
    // A successful TEST is only a status probe and preserves the last proven
    // frame. Every real skipped present, and every failed probe, fails open.
    return (flags & DXGI_PRESENT_TEST) == 0 || present_result < 0;
}

// Canonical SMAA performs perceptual edge detection from the stored sRGB
// values, but neighborhood blending in linear light. A typeless 8-bit resource
// permits both views without extra conversion passes.
[[nodiscard]] constexpr DXGI_FORMAT SmaaTypelessFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] constexpr DXGI_FORMAT SmaaLinearFormat(DXGI_FORMAT format) noexcept {
    switch (SmaaTypelessFormat(format)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] constexpr DXGI_FORMAT SmaaSrgbFormat(DXGI_FORMAT format) noexcept {
    switch (SmaaTypelessFormat(format)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool Initialize(std::uintptr_t module_base,
                std::uintptr_t device_slot,
                std::uintptr_t context_slot,
                std::uintptr_t swapchain_slot,
                std::uintptr_t present_rtv_slot);
// Opens DXGI hook discovery only after Hooks has committed its complete
// process-wide transaction. Initialize itself only prepares CPU-side state.
void Activate();
// Gates new SMAA work and releases D3D resources after active calls quiesce.
// The MinHook detours and their original trampolines are intentionally retained
// for process lifetime because this bundled fork has no safe disable-only API.
bool Shutdown();

void MaybeInstallHooks();

bool GetEnabled();
// True only when SMAA is enabled, both DXGI detours are owned, resources are
// live, and the current resource generation has completed a successful pass.
// Callers use this instead of GetEnabled() when deciding whether to skip the
// game's stock anti-aliasing path.
bool CanReplaceStockAa();
void SetEnabled(bool enabled);
bool GetDebugKeysEnabled();
void SetDebugKeysEnabled(bool enabled);
void SetPreset(int preset);

Stats GetStats();

}  // namespace spatch::smaa
