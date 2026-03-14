#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace spatch {

struct BuildCheckResult {
    bool supported = false;
    bool hook_layout_supported = false;
    std::string build_id;
    std::wstring exe_path;
    std::wstring version_text;
    unsigned short version_major = 0;
    unsigned short version_minor = 0;
    unsigned short version_build = 0;
    unsigned short version_private = 0;
    std::size_t file_size = 0;
    unsigned long time_date_stamp = 0;
    unsigned long size_of_image = 0;
    std::array<unsigned char, 32> sha256{};
    std::string summary;
};

BuildCheckResult InspectLoadedGame();

}  // namespace spatch
