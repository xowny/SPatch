#pragma once

#include "Config.h"

#include <filesystem>

namespace spatch {

void ApplyDisplaySettingsPatches(const std::filesystem::path& path, const Config& config);

}  // namespace spatch
