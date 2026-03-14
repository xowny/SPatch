#pragma once

#include <Windows.h>

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
    bool hooks_installed = false;
    bool resources_ready = false;
    bool enabled = false;
};

bool Initialize(std::uintptr_t module_base,
                std::uintptr_t device_slot,
                std::uintptr_t context_slot,
                std::uintptr_t swapchain_slot,
                std::uintptr_t present_rtv_slot);
void Shutdown();

void MaybeInstallHooks();

bool GetEnabled();
void SetEnabled(bool enabled);
bool GetDebugKeysEnabled();
void SetDebugKeysEnabled(bool enabled);
void SetPreset(int preset);

Stats GetStats();

}  // namespace spatch::smaa
