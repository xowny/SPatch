#pragma once

#include "Config.h"

#include <filesystem>
#include <string_view>

namespace spatch::hooks {

bool Initialize(const Config& config,
                const std::filesystem::path& config_path,
                const std::filesystem::path& display_settings_path,
                std::string_view build_id,
                bool hook_layout_supported);
void Shutdown();

}  // namespace spatch::hooks
