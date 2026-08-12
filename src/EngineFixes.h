#pragma once

#include "Config.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace spatch::engine_fixes {

struct ReflectionResolution {
    int width = kSphericalReflectionWidthMin;
    int height = kSphericalReflectionWidthMin / 2;
};

[[nodiscard]] ReflectionResolution ResolveReflectionResolution(int configured_width,
                                                               int detected_display_width);
[[nodiscard]] int DetectGameDisplayWidth(const std::filesystem::path& display_settings_path);
[[nodiscard]] bool IsSafeSavePayload(std::uintptr_t payload,
                                     std::uint32_t payload_size) noexcept;
[[nodiscard]] bool IsSafeSaveFile(std::uintptr_t file_data,
                                  std::uint32_t file_size) noexcept;
[[nodiscard]] std::uintptr_t NormalizeThreadHandle(std::uintptr_t handle) noexcept;
[[nodiscard]] bool AreTaskManagerEventsReady(std::uintptr_t sync_event,
                                             std::uintptr_t close_event,
                                             std::uintptr_t add_event,
                                             std::uintptr_t all_done_event) noexcept;
[[nodiscard]] bool AreIoThreadBootstrapEventsReady(std::uintptr_t wake_event,
                                                   std::uintptr_t shutdown_event,
                                                   std::uintptr_t work_event,
                                                   std::uintptr_t idle_event) noexcept;
[[nodiscard]] bool AreBankManagerEventsReady(
    std::uintptr_t wake_event,
    std::uintptr_t callback_fence_event) noexcept;
[[nodiscard]] std::uintptr_t SelectCreatedOrReservedEvent(
    std::uintptr_t created_event,
    std::uintptr_t reserved_event) noexcept;
[[nodiscard]] bool IsWwiseBlockingOperationPending(
    std::uint8_t pending_flag) noexcept;
[[nodiscard]] std::optional<std::int32_t> ComputeRelativeBranchDisplacement(
    std::uintptr_t instruction_address,
    std::size_t instruction_size,
    std::uintptr_t target_address) noexcept;

[[nodiscard]] bool InitializeStaticPatches(
    const Config& config,
    std::uintptr_t module_base,
    bool latest_steam_layout,
    bool sampler_builder_prevalidated,
    const std::filesystem::path& display_settings_path);
// Failed feature groups remain retryable while already-owned groups retain
// their rollback records; callers may invoke initialization again after a
// transient allocation/protection/signature failure.
// Returns false while an owned patch could not be restored.  Callers must keep
// the module/lifecycle state alive and retry instead of unloading MinHook or
// the DLL with executable mutations still owned by SPatch.
[[nodiscard]] bool ShutdownStaticPatches();
[[nodiscard]] bool IsInitialized();

}  // namespace spatch::engine_fixes
