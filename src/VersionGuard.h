#pragma once

#include <array>
#include <filesystem>
#include <string>

#include <Windows.h>

namespace spatch {

struct BuildCheckResult {
    bool supported = false;
    bool hook_layout_supported = false;
    bool hash_computed = false;
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

std::string IdentifyBuildFromPeMetadata(std::size_t file_size,
                                        unsigned long time_date_stamp,
                                        unsigned long size_of_image);
using ReadFileCallback =
    BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using ComputeSha256PathCallback =
    bool (*)(const std::filesystem::path&, std::array<unsigned char, 32>&);
bool ComputeSha256FromPath(const std::filesystem::path& path,
                           std::array<unsigned char, 32>& hash_out);
bool ComputeSha256FromPathWithReadCallback(const std::filesystem::path& path,
                                           std::array<unsigned char, 32>& hash_out,
                                           ReadFileCallback read_file_callback);
bool ShouldUseSafeCompatibilityMode(const BuildCheckResult& build, bool allow_unverified_build);
BuildCheckResult InspectGameAtPath(const std::filesystem::path& path);
BuildCheckResult InspectGameAtPathWithHashCallback(const std::filesystem::path& path,
                                                   ComputeSha256PathCallback hash_callback);
BuildCheckResult InspectLoadedGame();

}  // namespace spatch
