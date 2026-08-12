#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace spatch::graphics::ini {

struct Key {
    const wchar_t* section = nullptr;
    const wchar_t* name = nullptr;
};

inline constexpr Key kMasterEnabledKeys[] = {
    {L"ShenLong", L"Enabled"},
    {L"ShenLong", L"enabled"},
};

constexpr std::array<Key, 4> SettingKeys(
    const wchar_t* section,
    const wchar_t* canonical_name,
    const wchar_t* legacy_name) noexcept {
    return {{{section, canonical_name},
             {L"ShenLong", canonical_name},
             {section, legacy_name},
             {L"ShenLong", legacy_name}}};
}

constexpr std::array<Key, 8> ExtendedSettingKeys(
    const wchar_t* section,
    const wchar_t* canonical_name,
    const wchar_t* legacy_public_name,
    const wchar_t* internal_name,
    const wchar_t* legacy_internal_name) noexcept {
    return {{{section, canonical_name},
             {L"ShenLong", canonical_name},
             {section, legacy_public_name},
             {L"ShenLong", legacy_public_name},
             {section, internal_name},
             {L"ShenLong", internal_name},
             {section, legacy_internal_name},
             {L"ShenLong", legacy_internal_name}}};
}

inline std::wstring Trim(std::wstring value) {
    const std::size_t comment = value.find_first_of(L";#");
    if (comment != std::wstring::npos) {
        value.resize(comment);
    }
    const std::size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::optional<std::wstring> ReadValue(
    const std::wstring& path,
    const wchar_t* section,
    const wchar_t* name) {
    if (path.empty() || section == nullptr || name == nullptr ||
        *section == L'\0' || *name == L'\0') {
        return std::nullopt;
    }

    constexpr wchar_t missing[] = L"\x1";
    std::vector<wchar_t> buffer(128, L'\0');
    for (;;) {
        const DWORD length = GetPrivateProfileStringW(
            section,
            name,
            missing,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            path.c_str());
        if ((length == 1 && buffer[0] == missing[0]) || length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            return Trim(std::wstring(buffer.data(), length));
        }
        if (buffer.size() >= 32768) {
            return std::nullopt;
        }
        buffer.assign((std::min)(buffer.size() * 2, std::size_t{32768}), L'\0');
    }
}

inline std::optional<std::wstring> ReadFirst(
    const std::wstring& path,
    std::span<const Key> keys) {
    for (const Key& key : keys) {
        if (auto value = ReadValue(path, key.section, key.name)) {
            return value;
        }
    }
    return std::nullopt;
}

inline bool ParseBool(const std::optional<std::wstring>& raw, bool fallback) {
    if (!raw || raw->empty()) {
        return fallback;
    }
    if (*raw == L"1" || _wcsicmp(raw->c_str(), L"true") == 0 ||
        _wcsicmp(raw->c_str(), L"on") == 0 ||
        _wcsicmp(raw->c_str(), L"yes") == 0) {
        return true;
    }
    if (*raw == L"0" || _wcsicmp(raw->c_str(), L"false") == 0 ||
        _wcsicmp(raw->c_str(), L"off") == 0 ||
        _wcsicmp(raw->c_str(), L"no") == 0) {
        return false;
    }
    return fallback;
}

inline int ParseInt(const std::optional<std::wstring>& raw, int fallback) {
    if (!raw || raw->empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const long long value = std::wcstoll(raw->c_str(), &end, 10);
    if (errno == ERANGE || end == raw->c_str() || *end != L'\0' ||
        value < (std::numeric_limits<int>::min)() ||
        value > (std::numeric_limits<int>::max)()) {
        return fallback;
    }
    return static_cast<int>(value);
}

inline float ParseFloat(const std::optional<std::wstring>& raw, float fallback) {
    if (!raw || raw->empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const float value = std::wcstof(raw->c_str(), &end);
    return errno == 0 && end != raw->c_str() && *end == L'\0' &&
            std::isfinite(value)
        ? value
        : fallback;
}

inline bool ReadBool(
    const std::wstring& path,
    std::span<const Key> keys,
    bool fallback) {
    return ParseBool(ReadFirst(path, keys), fallback);
}

inline int ReadInt(
    const std::wstring& path,
    std::span<const Key> keys,
    int fallback) {
    return ParseInt(ReadFirst(path, keys), fallback);
}

inline float ReadFloat(
    const std::wstring& path,
    std::span<const Key> keys,
    float fallback) {
    return ParseFloat(ReadFirst(path, keys), fallback);
}

}  // namespace spatch::graphics::ini
