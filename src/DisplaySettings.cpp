#include "DisplaySettings.h"

#include "Logger.h"

#include <Windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

namespace spatch {
namespace {

std::atomic<unsigned long> g_display_write_serial = 0;

bool HasGogStorefrontMarker(const std::filesystem::path& game_root) {
    const std::filesystem::path pattern = game_root / L"goggame-*.info";
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool found_file = false;
    do {
        found_file = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    } while (!found_file && FindNextFileW(search, &entry));
    FindClose(search);
    return found_file;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0 || static_cast<unsigned long long>(size) >
                        static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()) ||
        static_cast<unsigned long long>(size) >
            static_cast<unsigned long long>(std::numeric_limits<std::streamsize>::max())) {
        return std::nullopt;
    }
    stream.seekg(0, std::ios::beg);

    std::string text(static_cast<std::size_t>(size), '\0');
    const auto expected_size = static_cast<std::streamsize>(size);
    stream.read(text.data(), expected_size);
    if (stream.gcount() != expected_size) {
        return std::nullopt;
    }

    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    const unsigned long serial = g_display_write_serial.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temp_path =
        path.parent_path() /
        (path.filename().wstring() + L".spatch." + std::to_wstring(GetCurrentProcessId()) +
         L"." + std::to_wstring(serial) + L".tmp");
    std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    stream.close();
    if (!stream) {
        std::error_code remove_error;
        std::filesystem::remove(temp_path, remove_error);
        return false;
    }

    if (MoveFileExW(temp_path.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return false;
}

std::optional<int> ReadIntTag(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";

    const std::size_t start = xml.find(open);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t value_start = start + open.size();
    const std::size_t value_end = xml.find(close, value_start);
    if (value_end == std::string_view::npos || value_end <= value_start) {
        return std::nullopt;
    }

    std::string_view value = xml.substr(value_start, value_end - value_start);
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    };
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return std::nullopt;
    }

    int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

bool SetIntTag(std::string& xml, std::string_view tag, int value) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const std::string replacement = open + std::to_string(value) + close;

    const std::size_t start = xml.find(open);
    if (start != std::string::npos) {
        const std::size_t value_start = start + open.size();
        const std::size_t end = xml.find(close, value_start);
        if (end == std::string::npos) {
            return false;
        }

        xml.replace(start, end + close.size() - start, replacement);
        return true;
    }

    const std::size_t root_end = xml.rfind("</DisplaySettings>");
    if (root_end == std::string::npos) {
        return false;
    }

    xml.insert(root_end, "\t" + replacement + "\n");
    return true;
}

bool UpdateIntTag(std::string& xml, std::string_view tag, int value, bool& changed) {
    const auto before = ReadIntTag(xml, tag);
    if (before.has_value() && *before == value) {
        return true;
    }
    if (!SetIntTag(xml, tag, value)) {
        return false;
    }
    changed = true;
    return true;
}

bool ApplyDisplayModePreferenceImpl(std::string& xml,
                                    const DisplayModePreference& preference,
                                    bool& changed) {
    if (preference.refresh_rate_numerator > 0) {
        const int refresh_denominator =
            preference.refresh_rate_denominator > 0 ? preference.refresh_rate_denominator : 1;
        if (!UpdateIntTag(
                xml, "RefreshRateNumerator", preference.refresh_rate_numerator, changed) ||
            !UpdateIntTag(xml, "RefreshRateDenominator", refresh_denominator, changed)) {
            return false;
        }
    }

    if (preference.fullscreen >= 0) {
        const int fullscreen = preference.fullscreen != 0 ? 1 : 0;
        if (!UpdateIntTag(xml, "Fullscreen", fullscreen, changed)) {
            return false;
        }
    }
    return true;
}

std::optional<DisplayRefreshRate> FindHighestRefreshRateForSelectedOutput(
    int width,
    int height,
    std::uint32_t adapter_uid,
    std::uint32_t monitor_uid) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }

    // DXGI normally terminates enumeration with DXGI_ERROR_NOT_FOUND. Keep a
    // hard upper bound as well so a broken/remote display driver that returns
    // another persistent failure cannot spin through the full UINT range at
    // game startup.
    constexpr UINT kMaximumAdapters = 64;
    constexpr UINT kMaximumOutputsPerAdapter = 256;
    ComPtr<IDXGIOutput> selected_output;
    DXGI_OUTPUT_DESC selected_output_desc{};
    for (UINT adapter_index = 0;
         adapter_index < kMaximumAdapters && !selected_output;
         ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapter_hr)) {
            continue;
        }

        DXGI_ADAPTER_DESC adapter_desc{};
        if (FAILED(adapter->GetDesc(&adapter_desc))) {
            continue;
        }
        static_assert(offsetof(DXGI_ADAPTER_DESC, VendorId) == 0x100);
        const auto adapter_identity = std::span{
            reinterpret_cast<const std::byte*>(&adapter_desc.VendorId), std::size_t{16}};
        if (ComputeGameDisplayUid(adapter_identity) != adapter_uid) {
            continue;
        }

        for (UINT output_index = 0;
             output_index < kMaximumOutputsPerAdapter;
             ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_hr = adapter->EnumOutputs(output_index, &output);
            if (output_hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(output_hr)) {
                continue;
            }

            DXGI_OUTPUT_DESC output_desc{};
            if (FAILED(output->GetDesc(&output_desc))) {
                continue;
            }
            static_assert(offsetof(DXGI_OUTPUT_DESC, Monitor) == 0x58);
            const auto output_identity = std::span{
                reinterpret_cast<const std::byte*>(&output_desc), offsetof(DXGI_OUTPUT_DESC, Monitor)};
            if (ComputeGameDisplayUid(output_identity) == monitor_uid) {
                selected_output = std::move(output);
                selected_output_desc = output_desc;
                break;
            }
        }
    }

    if (!selected_output) {
        log::WarnF("display_settings selected_output_not_found adapter_uid=%u monitor_uid=%u",
                   adapter_uid,
                   monitor_uid);
        return std::nullopt;
    }

    constexpr UINT kModeFlags = DXGI_ENUM_MODES_INTERLACED | DXGI_ENUM_MODES_SCALING;
    UINT mode_count = 0;
    HRESULT modes_hr = selected_output->GetDisplayModeList(
        DXGI_FORMAT_R8G8B8A8_UNORM, kModeFlags, &mode_count, nullptr);
    // The sizing call is normally documented to return DXGI_ERROR_MORE_DATA;
    // treating every failing HRESULT as fatal made UseMaximumRefreshRate a
    // silent no-op on drivers that follow that contract.
    if ((modes_hr != S_OK && modes_hr != DXGI_ERROR_MORE_DATA) || mode_count == 0 ||
        mode_count > 65536) {
        return std::nullopt;
    }

    std::vector<DXGI_MODE_DESC> dxgi_modes;
    try {
        dxgi_modes.resize(mode_count);
        // A mode can be added/removed between the sizing and fill calls.  Use
        // a bounded retry when the driver reports a larger required buffer;
        // never let a hostile driver grow this startup allocation unchecked.
        constexpr unsigned int kModeQueryAttempts = 2;
        bool filled = false;
        for (unsigned int attempt = 0; attempt < kModeQueryAttempts; ++attempt) {
            UINT capacity = static_cast<UINT>(dxgi_modes.size());
            UINT returned = capacity;
            modes_hr = selected_output->GetDisplayModeList(
                DXGI_FORMAT_R8G8B8A8_UNORM, kModeFlags, &returned, dxgi_modes.data());
            if (modes_hr == DXGI_ERROR_MORE_DATA && returned > capacity && returned <= 65536) {
                dxgi_modes.resize(returned);
                continue;
            }
            if (FAILED(modes_hr) || returned > dxgi_modes.size()) {
                return std::nullopt;
            }
            dxgi_modes.resize(returned);
            filled = true;
            break;
        }
        if (!filled) {
            return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }

    std::vector<DisplayModeCandidate> candidates;
    try {
        candidates.reserve(dxgi_modes.size());
        for (const DXGI_MODE_DESC& mode : dxgi_modes) {
            const bool progressive =
                mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED ||
                mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
            candidates.push_back(DisplayModeCandidate{
                .width = static_cast<int>(mode.Width),
                .height = static_cast<int>(mode.Height),
                .refresh_rate_numerator = mode.RefreshRate.Numerator,
                .refresh_rate_denominator = mode.RefreshRate.Denominator,
                .progressive = progressive,
            });
        }
    } catch (...) {
        return std::nullopt;
    }

    const auto selected = SelectHighestRefreshRate(candidates, width, height);
    if (selected.has_value()) {
        log::InfoF("display_settings selected_output=%s adapter_uid=%u monitor_uid=%u "
                   "max_refresh=%u/%u",
                   log::ToUtf8(selected_output_desc.DeviceName).c_str(),
                   adapter_uid,
                   monitor_uid,
                   selected->numerator,
                   selected->denominator);
    }
    return selected;
}

}  // namespace

bool ApplyDisplayModePreference(std::string& xml, const DisplayModePreference& preference) {
    // Work on a candidate so malformed XML cannot leave the caller with only
    // the first of several requested fields changed.
    std::string candidate = xml;
    bool changed = false;
    if (!ApplyDisplayModePreferenceImpl(candidate, preference, changed)) {
        return false;
    }
    if (changed) {
        xml.swap(candidate);
    }
    return changed;
}

std::uint32_t ComputeGameDisplayUid(std::span<const std::byte> bytes) {
    // The engine uses the non-reflected CRC-32/MPEG-2 update with an all-ones
    // initial value and no final xor for AdapterUID and MonitorUID.
    std::uint32_t hash = UINT32_MAX;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(value)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            hash = (hash & 0x80000000u) != 0 ? (hash << 1) ^ 0x04C11DB7u : hash << 1;
        }
    }
    return hash;
}

std::optional<DisplayRefreshRate> SelectHighestRefreshRate(
    std::span<const DisplayModeCandidate> modes,
    int width,
    int height) {
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }

    DisplayRefreshRate best{};
    for (const DisplayModeCandidate& mode : modes) {
        if (mode.width != width || mode.height != height || !mode.progressive ||
            mode.refresh_rate_denominator == 0 ||
            mode.refresh_rate_numerator <= mode.refresh_rate_denominator) {
            continue;
        }

        const std::uint64_t candidate_scaled =
            static_cast<std::uint64_t>(mode.refresh_rate_numerator) * best.denominator;
        const std::uint64_t best_scaled =
            static_cast<std::uint64_t>(best.numerator) * mode.refresh_rate_denominator;
        if (best.denominator == 0 || candidate_scaled > best_scaled ||
            (candidate_scaled == best_scaled &&
             mode.refresh_rate_denominator < best.denominator)) {
            best.numerator = mode.refresh_rate_numerator;
            best.denominator = mode.refresh_rate_denominator;
        }
    }

    return best.denominator != 0 ? std::optional<DisplayRefreshRate>{best} : std::nullopt;
}

std::filesystem::path ResolveDisplaySettingsPath(
    const std::filesystem::path& game_root) {
    const std::filesystem::path gog_path =
        game_root / L"Save" / L"DisplaySettings.xml";
    const std::filesystem::path steam_path =
        game_root / L"data" / L"DisplaySettings.xml";

    std::error_code error;
    if (std::filesystem::is_regular_file(gog_path, error)) {
        return gog_path;
    }

    // GOG's install marker disambiguates a first run (or a damaged settings
    // file) even if data\DisplaySettings.xml is also present. A plain Save
    // directory is not sufficient here because a Steam user may create one.
    if (HasGogStorefrontMarker(game_root)) {
        return gog_path;
    }

    error.clear();
    if (std::filesystem::is_regular_file(steam_path, error)) {
        return steam_path;
    }

    return steam_path;
}

std::optional<int> ReadDisplayResolutionWidth(const std::filesystem::path& path) {
    const auto xml = ReadTextFile(path);
    if (!xml.has_value()) {
        return std::nullopt;
    }
    const auto width = ReadIntTag(*xml, "ResolutionWidth");
    if (!width.has_value() || *width <= 0) {
        return std::nullopt;
    }
    return width;
}

void ApplyDisplaySettingsPatches(const std::filesystem::path& path, const Config& config) {
    const auto xml_opt = ReadTextFile(path);
    if (!xml_opt.has_value()) {
        log::WarnF("display_settings missing_or_unreadable path=%s",
                   log::ToUtf8(path.c_str()).c_str());
        return;
    }

    std::string xml = *xml_opt;
    const auto smoothing_before = ReadIntTag(xml, "TimeStepSmoothingFrames");
    const auto low_res_before = ReadIntTag(xml, "EnableLowResBuffer");
    const auto shadow_filter_before = ReadIntTag(xml, "ShadowFilter");
    const auto fps_limiter_before = ReadIntTag(xml, "FPSLimiter");
    const auto texture_detail_before = ReadIntTag(xml, "TextureDetailLevel");
    const auto world_density_before = ReadIntTag(xml, "WorldDensity");
    const auto motion_blur_before = ReadIntTag(xml, "MotionBlur");
    const auto refresh_numerator_before = ReadIntTag(xml, "RefreshRateNumerator");
    const auto refresh_denominator_before = ReadIntTag(xml, "RefreshRateDenominator");
    const auto fullscreen_before = ReadIntTag(xml, "Fullscreen");

    bool changed = false;

    const auto update_setting = [&](bool requested, std::string_view tag, int value) {
        return !requested || UpdateIntTag(xml, tag, value, changed);
    };
    const int original_shadow_filter = config.override_shadow_filter >= 0
                                           ? config.override_shadow_filter
                                           : shadow_filter_before.value_or(1);
    if (!update_setting(config.time_step_smoothing >= 0,
                        "TimeStepSmoothingFrames",
                        config.time_step_smoothing == 0 ? 0 : 2) ||
        !update_setting(config.override_low_res_buffer >= 0,
                        "EnableLowResBuffer",
                        config.override_low_res_buffer) ||
        !update_setting(config.override_shadow_filter >= 0,
                        "ShadowFilter",
                        original_shadow_filter) ||
        !update_setting(
            config.override_fps_limiter >= 0, "FPSLimiter", config.override_fps_limiter) ||
        !update_setting(config.override_texture_detail_level >= 0,
                        "TextureDetailLevel",
                        config.override_texture_detail_level) ||
        !update_setting(
            config.override_world_density >= 0, "WorldDensity", config.override_world_density) ||
        !update_setting(
            config.override_motion_blur >= 0, "MotionBlur", config.override_motion_blur)) {
        log::ErrorF("display_settings malformed_xml path=%s",
                    log::ToUtf8(path.c_str()).c_str());
        return;
    }

    DisplayModePreference preference{};
    if (config.prefer_max_refresh_rate) {
        const auto width = ReadIntTag(xml, "ResolutionWidth");
        const auto height = ReadIntTag(xml, "ResolutionHeight");
        const auto adapter_uid = ReadIntTag(xml, "AdapterUID");
        const auto monitor_uid = ReadIntTag(xml, "MonitorUID");
        if (width.has_value() && height.has_value() && adapter_uid.has_value() &&
            monitor_uid.has_value()) {
            const auto max_refresh_rate = FindHighestRefreshRateForSelectedOutput(
                *width,
                *height,
                static_cast<std::uint32_t>(*adapter_uid),
                static_cast<std::uint32_t>(*monitor_uid));
            if (max_refresh_rate.has_value()) {
                if (max_refresh_rate->numerator <=
                        static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) &&
                    max_refresh_rate->denominator <=
                        static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
                    preference.refresh_rate_numerator =
                        static_cast<int>(max_refresh_rate->numerator);
                    preference.refresh_rate_denominator =
                        static_cast<int>(max_refresh_rate->denominator);
                }
            }
        }
    }
    preference.fullscreen = config.override_fullscreen;
    bool preference_changed = false;
    if (!ApplyDisplayModePreferenceImpl(xml, preference, preference_changed)) {
        log::ErrorF("display_settings malformed_xml path=%s",
                    log::ToUtf8(path.c_str()).c_str());
        return;
    }
    changed |= preference_changed;

    if (changed) {
        if (!WriteTextFile(path, xml)) {
            log::ErrorF("display_settings write_failed path=%s",
                        log::ToUtf8(path.c_str()).c_str());
            return;
        }
    }

    const auto smoothing_after = ReadIntTag(xml, "TimeStepSmoothingFrames");
    const auto low_res_after = ReadIntTag(xml, "EnableLowResBuffer");
    const auto shadow_filter_after = ReadIntTag(xml, "ShadowFilter");
    const auto fps_limiter_after = ReadIntTag(xml, "FPSLimiter");
    const auto texture_detail_after = ReadIntTag(xml, "TextureDetailLevel");
    const auto world_density_after = ReadIntTag(xml, "WorldDensity");
    const auto motion_blur_after = ReadIntTag(xml, "MotionBlur");
    const auto refresh_numerator_after = ReadIntTag(xml, "RefreshRateNumerator");
    const auto refresh_denominator_after = ReadIntTag(xml, "RefreshRateDenominator");
    const auto fullscreen_after = ReadIntTag(xml, "Fullscreen");
    const std::string smoothing_before_text =
        smoothing_before.has_value() ? std::to_string(*smoothing_before) : "<missing>";
    const std::string smoothing_after_text =
        smoothing_after.has_value() ? std::to_string(*smoothing_after) : "<missing>";
    const std::string low_res_before_text =
        low_res_before.has_value() ? std::to_string(*low_res_before) : "<missing>";
    const std::string low_res_after_text =
        low_res_after.has_value() ? std::to_string(*low_res_after) : "<missing>";
    const std::string shadow_filter_before_text =
        shadow_filter_before.has_value() ? std::to_string(*shadow_filter_before) : "<missing>";
    const std::string shadow_filter_after_text =
        shadow_filter_after.has_value() ? std::to_string(*shadow_filter_after) : "<missing>";
    const std::string fps_limiter_before_text =
        fps_limiter_before.has_value() ? std::to_string(*fps_limiter_before) : "<missing>";
    const std::string fps_limiter_after_text =
        fps_limiter_after.has_value() ? std::to_string(*fps_limiter_after) : "<missing>";
    const std::string texture_detail_before_text =
        texture_detail_before.has_value() ? std::to_string(*texture_detail_before) : "<missing>";
    const std::string texture_detail_after_text =
        texture_detail_after.has_value() ? std::to_string(*texture_detail_after) : "<missing>";
    const std::string world_density_before_text =
        world_density_before.has_value() ? std::to_string(*world_density_before) : "<missing>";
    const std::string world_density_after_text =
        world_density_after.has_value() ? std::to_string(*world_density_after) : "<missing>";
    const std::string motion_blur_before_text =
        motion_blur_before.has_value() ? std::to_string(*motion_blur_before) : "<missing>";
    const std::string motion_blur_after_text =
        motion_blur_after.has_value() ? std::to_string(*motion_blur_after) : "<missing>";
    const std::string refresh_numerator_before_text =
        refresh_numerator_before.has_value() ? std::to_string(*refresh_numerator_before) : "<missing>";
    const std::string refresh_numerator_after_text =
        refresh_numerator_after.has_value() ? std::to_string(*refresh_numerator_after) : "<missing>";
    const std::string refresh_denominator_before_text =
        refresh_denominator_before.has_value() ? std::to_string(*refresh_denominator_before) : "<missing>";
    const std::string refresh_denominator_after_text =
        refresh_denominator_after.has_value() ? std::to_string(*refresh_denominator_after) : "<missing>";
    const std::string fullscreen_before_text =
        fullscreen_before.has_value() ? std::to_string(*fullscreen_before) : "<missing>";
    const std::string fullscreen_after_text =
        fullscreen_after.has_value() ? std::to_string(*fullscreen_after) : "<missing>";

    log::InfoF(
        "display_settings path=%s changed=%d smoothing_before=%s smoothing_after=%s "
        "low_res_before=%s low_res_after=%s shadow_filter_before=%s shadow_filter_after=%s "
        "fps_limiter_before=%s fps_limiter_after=%s "
        "texture_detail_before=%s texture_detail_after=%s world_density_before=%s "
        "world_density_after=%s motion_blur_before=%s motion_blur_after=%s "
        "refresh_num_before=%s refresh_num_after=%s "
        "refresh_den_before=%s refresh_den_after=%s fullscreen_before=%s fullscreen_after=%s",
        log::ToUtf8(path.c_str()).c_str(),
        changed ? 1 : 0,
        smoothing_before_text.c_str(),
        smoothing_after_text.c_str(),
        low_res_before_text.c_str(),
        low_res_after_text.c_str(),
        shadow_filter_before_text.c_str(),
        shadow_filter_after_text.c_str(),
        fps_limiter_before_text.c_str(),
        fps_limiter_after_text.c_str(),
        texture_detail_before_text.c_str(),
        texture_detail_after_text.c_str(),
        world_density_before_text.c_str(),
        world_density_after_text.c_str(),
        motion_blur_before_text.c_str(),
        motion_blur_after_text.c_str(),
        refresh_numerator_before_text.c_str(),
        refresh_numerator_after_text.c_str(),
        refresh_denominator_before_text.c_str(),
        refresh_denominator_after_text.c_str(),
        fullscreen_before_text.c_str(),
        fullscreen_after_text.c_str());

    if (config.warn_low_res_buffer && low_res_after.has_value() && *low_res_after != 0) {
        log::Warn("EnableLowResBuffer is active; this is a known quality footgun");
    }
}


}  // namespace spatch
