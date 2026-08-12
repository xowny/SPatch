#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace spatch::cut_content {

constexpr std::uint32_t SymbolHash(std::string_view text) noexcept {
    std::uint32_t hash = 0xFFFFFFFFu;
    for (const unsigned char character : text) {
        hash ^= static_cast<std::uint32_t>(character) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            hash = (hash & 0x80000000u) != 0u ? (hash << 1) ^ 0x04C11DB7u
                                                : hash << 1;
        }
    }
    return hash;
}

void Initialize(std::uintptr_t module_base,
                bool latest_steam_layout,
                const std::filesystem::path& trigger_path) noexcept;
bool IsArmed() noexcept;
void OnGameThreadFrame() noexcept;
void Shutdown() noexcept;

}  // namespace spatch::cut_content
