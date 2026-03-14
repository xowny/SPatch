#include "DisplaySettings.h"

#include "Logger.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>
#include <string>
#include <string_view>

namespace spatch {
namespace {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    stream.seekg(0, std::ios::beg);

    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), size);
    if (!stream && !stream.eof()) {
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
    const std::filesystem::path temp_path = path.parent_path() / (path.filename().wstring() + L".spatch.tmp");
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

    std::error_code rename_error;
    std::filesystem::rename(temp_path, path, rename_error);
    if (!rename_error) {
        return true;
    }

    std::filesystem::remove(path, rename_error);
    rename_error.clear();
    std::filesystem::rename(temp_path, path, rename_error);
    if (!rename_error) {
        return true;
    }

    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return false;
}

std::size_t FindTagStart(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    return xml.find(open);
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

    try {
        return std::stoi(std::string(xml.substr(value_start, value_end - value_start)));
    } catch (...) {
        return std::nullopt;
    }
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

}  // namespace

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
    const auto ssao_before = ReadIntTag(xml, "SSAO");
    const auto fps_limiter_before = ReadIntTag(xml, "FPSLimiter");
    const auto texture_detail_before = ReadIntTag(xml, "TextureDetailLevel");
    const auto world_density_before = ReadIntTag(xml, "WorldDensity");

    bool changed = false;

    if (config.time_step_smoothing_frames >= 0 &&
        (!smoothing_before.has_value() || *smoothing_before != config.time_step_smoothing_frames)) {
        changed |= SetIntTag(xml, "TimeStepSmoothingFrames", config.time_step_smoothing_frames);
    }

    if (config.override_low_res_buffer >= 0 &&
        (!low_res_before.has_value() || *low_res_before != config.override_low_res_buffer)) {
        changed |= SetIntTag(xml, "EnableLowResBuffer", config.override_low_res_buffer);
    }

    if (config.override_shadow_filter >= 0 &&
        (!shadow_filter_before.has_value() ||
         *shadow_filter_before != config.override_shadow_filter)) {
        changed |= SetIntTag(xml, "ShadowFilter", config.override_shadow_filter);
    }

    if (config.override_ssao >= 0 &&
        (!ssao_before.has_value() || *ssao_before != config.override_ssao)) {
        changed |= SetIntTag(xml, "SSAO", config.override_ssao);
    }

    if (config.override_fps_limiter >= 0 &&
        (!fps_limiter_before.has_value() || *fps_limiter_before != config.override_fps_limiter)) {
        changed |= SetIntTag(xml, "FPSLimiter", config.override_fps_limiter);
    }

    if (config.override_texture_detail_level >= 0 &&
        (!texture_detail_before.has_value() ||
         *texture_detail_before != config.override_texture_detail_level)) {
        changed |= SetIntTag(xml, "TextureDetailLevel", config.override_texture_detail_level);
    }

    if (config.override_world_density >= 0 &&
        (!world_density_before.has_value() || *world_density_before != config.override_world_density)) {
        changed |= SetIntTag(xml, "WorldDensity", config.override_world_density);
    }

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
    const auto ssao_after = ReadIntTag(xml, "SSAO");
    const auto fps_limiter_after = ReadIntTag(xml, "FPSLimiter");
    const auto texture_detail_after = ReadIntTag(xml, "TextureDetailLevel");
    const auto world_density_after = ReadIntTag(xml, "WorldDensity");
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
    const std::string ssao_before_text =
        ssao_before.has_value() ? std::to_string(*ssao_before) : "<missing>";
    const std::string ssao_after_text =
        ssao_after.has_value() ? std::to_string(*ssao_after) : "<missing>";
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

    log::InfoF(
        "display_settings path=%s changed=%d smoothing_before=%s smoothing_after=%s "
        "low_res_before=%s low_res_after=%s shadow_filter_before=%s shadow_filter_after=%s "
        "ssao_before=%s ssao_after=%s fps_limiter_before=%s fps_limiter_after=%s "
        "texture_detail_before=%s texture_detail_after=%s world_density_before=%s "
        "world_density_after=%s",
        log::ToUtf8(path.c_str()).c_str(),
        changed ? 1 : 0,
        smoothing_before_text.c_str(),
        smoothing_after_text.c_str(),
        low_res_before_text.c_str(),
        low_res_after_text.c_str(),
        shadow_filter_before_text.c_str(),
        shadow_filter_after_text.c_str(),
        ssao_before_text.c_str(),
        ssao_after_text.c_str(),
        fps_limiter_before_text.c_str(),
        fps_limiter_after_text.c_str(),
        texture_detail_before_text.c_str(),
        texture_detail_after_text.c_str(),
        world_density_before_text.c_str(),
        world_density_after_text.c_str());

    if (config.warn_low_res_buffer && low_res_after.has_value() && *low_res_after != 0) {
        log::Warn("EnableLowResBuffer is active; this is a known quality footgun");
    }
}

}  // namespace spatch
