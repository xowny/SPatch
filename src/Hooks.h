#pragma once

#include "Config.h"

#include <filesystem>
#include <string_view>

namespace spatch::hooks {

bool Initialize(const Config& config,
                const std::filesystem::path& config_path,
                std::string_view build_id);
void Shutdown();

bool GetAcesEnabled();
void SetAcesEnabled(bool enabled);
int GetAcesStrengthPercent();
void SetAcesStrengthPercent(int strength_percent);
int GetAcesExposureScalePercent();
void SetAcesExposureScalePercent(int exposure_scale_percent);

}  // namespace spatch::hooks
