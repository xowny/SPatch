#pragma once

#include "Config.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace spatch {

struct DisplayModePreference {
    int refresh_rate_numerator = 0;
    int refresh_rate_denominator = 0;
    int fullscreen = -1;
};

struct DisplayRefreshRate {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
};

struct DisplayModeCandidate {
    int width = 0;
    int height = 0;
    std::uint32_t refresh_rate_numerator = 0;
    std::uint32_t refresh_rate_denominator = 0;
    bool progressive = false;
};

bool ApplyDisplayModePreference(std::string& xml, const DisplayModePreference& preference);
std::uint32_t ComputeGameDisplayUid(std::span<const std::byte> bytes);
std::optional<DisplayRefreshRate> SelectHighestRefreshRate(
    std::span<const DisplayModeCandidate> modes,
    int width,
    int height);
// Steam stores this file under data, while the GOG build stores it under
// Save. Resolve the live file once and pass the result through startup so all
// settings and static-fix consumers use the same operating path.
std::filesystem::path ResolveDisplaySettingsPath(
    const std::filesystem::path& game_root);
std::optional<int> ReadDisplayResolutionWidth(const std::filesystem::path& path);
void ApplyDisplaySettingsPatches(const std::filesystem::path& path, const Config& config);

}  // namespace spatch
