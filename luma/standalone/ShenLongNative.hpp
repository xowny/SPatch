#pragma once

#include <Windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace spatch::graphics::native {

inline constexpr int kShenLongConfigVersion = 1;

enum class AoMode {
    Original,
    Sdao,
    GtaoLite,
};

struct ExecutableProfile {
    const char* id = nullptr;
    std::size_t file_size = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t size_of_image = 0;
    std::array<std::uint8_t, 32> sha256{};
    std::uint32_t ao_stage_rva = 0;
    std::array<std::uint8_t, 32> ao_stage_signature{};
    std::uint32_t hair_blur_submit_rva = 0;
    std::array<std::uint8_t, 32> hair_blur_submit_signature{};
};

inline constexpr std::array<std::uint8_t, 32> kLegacySha256 = {
    0xC6, 0xDB, 0x19, 0x9B, 0x76, 0x92, 0xD2, 0x42,
    0x31, 0xC2, 0x16, 0xFC, 0x29, 0xDC, 0x43, 0x0E,
    0xC3, 0xAF, 0xD5, 0x94, 0x35, 0xAD, 0x5C, 0x1A,
    0xC5, 0x89, 0x93, 0x4B, 0xE8, 0xCC, 0x60, 0x35};

inline constexpr std::array<std::uint8_t, 32> kLatestSteamSha256 = {
    0x2A, 0x33, 0xEC, 0x78, 0x7A, 0xC6, 0xFD, 0x4C,
    0x86, 0xFE, 0xC2, 0xB6, 0xF7, 0x78, 0xFE, 0xEA,
    0x88, 0x1A, 0x3F, 0x35, 0xEA, 0x56, 0xC6, 0x80,
    0x12, 0x1F, 0x53, 0x57, 0x1C, 0x05, 0x27, 0xDA};

inline constexpr std::array<std::uint8_t, 32> kLegacyAoStageSignature = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xEF,
    0xFF, 0xFF, 0xB8, 0xF0, 0x11, 0x00, 0x00, 0xE8,
    0x44, 0x8F, 0x27, 0x01, 0x48, 0x2B, 0xE0, 0x48};

inline constexpr std::array<std::uint8_t, 32> kLatestAoStageSignature = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xEF,
    0xFF, 0xFF, 0xB8, 0xF0, 0x11, 0x00, 0x00, 0xE8,
    0xE4, 0x88, 0x27, 0x01, 0x48, 0x2B, 0xE0, 0x48};

inline constexpr std::array<std::uint8_t, 32> kHairBlurSubmitSignature = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x28,
    0xF2, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x0E,
    0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x70, 0xFE};

inline constexpr ExecutableProfile kLegacyProfile = {
    "legacy_researched",
    37490688,
    0x5408D5E9,
    0x02765000,
    kLegacySha256,
    0x00035370,
    kLegacyAoStageSignature,
    0x0003E7C0,
    kHairBlurSubmitSignature,
};

inline constexpr ExecutableProfile kLatestSteamProfile = {
    "latest_steam",
    37490688,
    0x543D6BB0,
    0x02765000,
    kLatestSteamSha256,
    0x00035650,
    kLatestAoStageSignature,
    0x0003EA60,
    kHairBlurSubmitSignature,
};

inline constexpr std::array<ExecutableProfile, 2> kExecutableProfiles = {
    kLegacyProfile,
    kLatestSteamProfile,
};

constexpr const ExecutableProfile* FindExecutableProfile(
    std::size_t file_size,
    std::uint32_t timestamp,
    std::uint32_t size_of_image,
    const std::array<std::uint8_t, 32>& sha256) noexcept {
    for (const ExecutableProfile& profile : kExecutableProfiles) {
        if (profile.file_size == file_size &&
            profile.timestamp == timestamp &&
            profile.size_of_image == size_of_image &&
            profile.sha256 == sha256) {
            return &profile;
        }
    }
    return nullptr;
}

constexpr wchar_t FoldAscii(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ?
        static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

constexpr bool EqualsAsciiInsensitive(
    std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (FoldAscii(left[index]) != FoldAscii(right[index])) {
            return false;
        }
    }
    return true;
}

constexpr AoMode ParseAoMode(
    std::wstring_view value, AoMode fallback = AoMode::Original) noexcept {
    if (EqualsAsciiInsensitive(value, L"Original") ||
        EqualsAsciiInsensitive(value, L"Native") || value == L"0" ||
        EqualsAsciiInsensitive(value, L"false") ||
        EqualsAsciiInsensitive(value, L"off") ||
        EqualsAsciiInsensitive(value, L"no")) {
        return AoMode::Original;
    }
    if (EqualsAsciiInsensitive(value, L"SDAO") ||
        EqualsAsciiInsensitive(value, L"GTAO") || value == L"1" ||
        EqualsAsciiInsensitive(value, L"true") ||
        EqualsAsciiInsensitive(value, L"on") ||
        EqualsAsciiInsensitive(value, L"yes")) {
        return AoMode::Sdao;
    }
    if (EqualsAsciiInsensitive(value, L"GTAOLite") ||
        EqualsAsciiInsensitive(value, L"GTAO-Lite") ||
        EqualsAsciiInsensitive(value, L"GTAO_Lite") || value == L"2") {
        return AoMode::GtaoLite;
    }
    return fallback;
}

constexpr bool UsesCustomAo(AoMode mode) noexcept {
    return mode != AoMode::Original;
}

constexpr int ValidateOriginalAoQuality(int value) noexcept {
    return value >= -1 && value <= 1 ? value : -1;
}

enum class SsaoXmlStatus {
    Ok,
    Malformed,
};

struct SsaoXmlInspection {
    SsaoXmlStatus status = SsaoXmlStatus::Malformed;
    std::optional<int> value;
    std::size_t value_begin = std::string_view::npos;
    std::size_t value_end = std::string_view::npos;
    std::size_t root_end = std::string_view::npos;
};

struct SsaoXmlEdit {
    SsaoXmlStatus status = SsaoXmlStatus::Malformed;
    std::optional<int> previous_value;
    bool changed = false;
    std::string text;
};

constexpr std::size_t CountOccurrences(
    std::string_view text, std::string_view needle) noexcept {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = text.find(needle, cursor)) != std::string_view::npos) {
        ++count;
        cursor += needle.size();
    }
    return count;
}

inline SsaoXmlInspection InspectSsaoXml(std::string_view xml) noexcept {
    constexpr std::string_view kOpen = "<SSAO>";
    constexpr std::string_view kClose = "</SSAO>";
    constexpr std::string_view kRootClose = "</DisplaySettings>";

    SsaoXmlInspection result;
    const std::size_t opens = CountOccurrences(xml, kOpen);
    const std::size_t closes = CountOccurrences(xml, kClose);
    const std::size_t roots = CountOccurrences(xml, kRootClose);
    if (roots != 1 || opens > 1 || closes > 1 || opens != closes) {
        return result;
    }

    result.root_end = xml.find(kRootClose);
    if (opens == 0) {
        result.status = SsaoXmlStatus::Ok;
        return result;
    }

    const std::size_t open = xml.find(kOpen);
    const std::size_t begin = open + kOpen.size();
    const std::size_t end = xml.find(kClose, begin);
    if (end == std::string_view::npos || end > result.root_end) {
        return result;
    }

    std::string_view raw = xml.substr(begin, end - begin);
    while (!raw.empty() &&
           (raw.front() == ' ' || raw.front() == '\t' ||
            raw.front() == '\r' || raw.front() == '\n')) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() &&
           (raw.back() == ' ' || raw.back() == '\t' ||
            raw.back() == '\r' || raw.back() == '\n')) {
        raw.remove_suffix(1);
    }

    if (!raw.empty()) {
        int parsed = 0;
        const auto [parsed_end, error] =
            std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
        if (error == std::errc{} && parsed_end == raw.data() + raw.size()) {
            result.value = parsed;
        }
    }
    result.value_begin = begin;
    result.value_end = end;
    result.status = SsaoXmlStatus::Ok;
    return result;
}

inline SsaoXmlEdit EditSsaoXml(std::string_view xml, int quality) {
    SsaoXmlEdit result;
    const SsaoXmlInspection inspection = InspectSsaoXml(xml);
    result.status = inspection.status;
    result.previous_value = inspection.value;
    if (quality != 0 && quality != 1) {
        result.status = SsaoXmlStatus::Malformed;
        return result;
    }
    if (inspection.status != SsaoXmlStatus::Ok) {
        return result;
    }

    result.text.assign(xml);
    if (inspection.value_begin != std::string_view::npos) {
        if (inspection.value.has_value() && *inspection.value == quality) {
            return result;
        }
        result.text.replace(
            inspection.value_begin,
            inspection.value_end - inspection.value_begin,
            quality == 0 ? "0" : "1");
        result.changed = true;
        return result;
    }

    const bool uses_crlf = xml.find("\r\n") != std::string_view::npos;
    const std::string insertion =
        std::string("\t<SSAO>") + (quality == 0 ? "0" : "1") +
        "</SSAO>" + (uses_crlf ? "\r\n" : "\n");
    result.text.insert(inspection.root_end, insertion);
    result.changed = true;
    return result;
}

inline std::optional<int> ParseRestoreJournal(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' ||
            text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    if (text == "0") {
        return 0;
    }
    if (text == "1") {
        return 1;
    }
    return std::nullopt;
}

// Called synchronously from ReShade's pre-D3D11 create_device event only after
// the loaded executable has matched one exact supported SHA-256 and PE
// identity. Returning false keeps the verified identity unpublished and makes
// every prepared native hook transparent.
using VerifiedTargetCallback = bool (*)(
    HMODULE module, const ExecutableProfile& profile) noexcept;

// Called from DllMain to register one create_device callback. File inspection,
// hashing, component registration, settings staging, and fixed-RVA hook work
// execute once from that callback after loader lock is released and before the
// real D3D11 device is created. The image is pinned only after exact executable
// verification succeeds.
bool Attach(HMODULE module, VerifiedTargetCallback on_verified) noexcept;

// Published only after exact SHA-256/PE verification and complete renderer
// callback registration. Other ShenLong native components use this to fail
// closed instead of authorizing signature-scanned ABIs on an unknown
// executable.
const ExecutableProfile* GetVerifiedExecutableProfile() noexcept;

// Process termination closes behavioral gates without waiting or performing
// file I/O. A non-terminating detach additionally restores any pending AO
// staging and performs a bounded accepted-call drain. Installed MinHook
// trampolines remain pinned for process lifetime.
void Detach(bool process_terminating) noexcept;

}  // namespace spatch::graphics::native
