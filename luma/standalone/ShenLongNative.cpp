#include "ShenLongNative.hpp"

#include "SPatchIni.hpp"
#include "../../src/HookTargetGuard.h"

#include <MinHook.h>
#include <reshade.hpp>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace spatch::graphics::native {
namespace {

constexpr wchar_t kExecutableName[] = L"sdhdship.exe";
constexpr wchar_t kModuleName[] = L"ShenLong.asi";
constexpr wchar_t kConfigName[] = L"ShenLong.ini";
constexpr wchar_t kRestoreJournalName[] = L"ShenLong-AO-Restore.pending";
constexpr std::size_t kMaximumSettingsBytes = 4u * 1024u * 1024u;
constexpr DWORD kRestoreFallbackDelayMilliseconds = 15000;
constexpr DWORD kRestoreRetryDelayMilliseconds = 250;
constexpr unsigned int kRestoreMaximumRetries = 8;
constexpr DWORD kDetachDrainMilliseconds = 250;

using AmbientOcclusionStageFn =
    void (*)(char*, std::uintptr_t, long long*);
using HairBlurSubmitFn = void (*)(std::uintptr_t,
                                  std::uintptr_t,
                                  std::uintptr_t,
                                  std::uintptr_t,
                                  std::uintptr_t,
                                  std::uintptr_t);

struct Settings {
    int config_version = 0;
    bool enabled = false;
    AoMode ao_mode = AoMode::Original;
    int original_ao_quality = -1;
    bool stock_hair_blur = false;
};

struct LoadedPaths {
    std::filesystem::path executable;
    std::filesystem::path module;
    std::filesystem::path game_root;
    std::filesystem::path config;
    std::filesystem::path display_settings;
    std::filesystem::path restore_journal;
};

struct ExecutableIdentityObservation {
    bool hash_read = false;
    bool pe_read = false;
    std::size_t file_size = 0;
    std::array<std::uint8_t, 32> sha256{};
    std::uint32_t timestamp = 0;
    std::uint32_t size_of_image = 0;
};

enum class ReadStatus {
    Ok,
    Missing,
    Rejected,
};

HMODULE g_module = nullptr;
VerifiedTargetCallback g_on_verified = nullptr;
std::atomic<const ExecutableProfile*> g_verified_profile = nullptr;
std::atomic<bool> g_started = false;
std::atomic<bool> g_stop_requested = false;
std::atomic<bool> g_behavior_accepting = false;
std::atomic<unsigned int> g_accepted_calls = 0;
std::atomic<bool> g_custom_ao_enabled = false;
std::atomic<bool> g_hair_blur_suppression_enabled = false;
std::atomic<bool> g_ao_restore_requested = false;
std::atomic<bool> g_ao_live_scheduler_logged = false;
std::atomic<bool> g_ao_live_scheduler_warning_logged = false;
std::atomic<bool> g_hair_blur_suppression_logged = false;
std::atomic<unsigned long> g_atomic_write_serial = 0;
std::once_flag g_bootstrap_once;
AmbientOcclusionStageFn g_ao_stage_original = nullptr;
HairBlurSubmitFn g_hair_blur_submit_original = nullptr;

std::mutex g_restore_mutex;
std::filesystem::path g_restore_display_path;
std::filesystem::path g_restore_journal_path;
std::optional<int> g_restore_quality;
PTP_TIMER g_restore_timer = nullptr;
unsigned int g_restore_retry_count = 0;

void Log(reshade::log::level level, const char* format, ...) noexcept {
    std::array<char, 1024> message{};
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(message.data(), message.size(), _TRUNCATE, format, arguments);
    va_end(arguments);
    reshade::log::message(level, message.data());
}

bool PathNameEquals(const std::filesystem::path& path,
                    const wchar_t* expected) noexcept {
    try {
        const std::wstring name = path.filename().wstring();
        return !name.empty() && expected != nullptr &&
            CompareStringOrdinal(name.c_str(), -1, expected, -1, TRUE) ==
                CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

bool PathsEqual(const std::filesystem::path& left,
                const std::filesystem::path& right) noexcept {
    try {
        const std::wstring left_text = left.lexically_normal().wstring();
        const std::wstring right_text = right.lexically_normal().wstring();
        return !left_text.empty() &&
            CompareStringOrdinal(left_text.c_str(), -1,
                                 right_text.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

bool IsAbsoluteFilePath(const std::filesystem::path& path) noexcept {
    try {
        return !path.empty() && path.is_absolute() && path.has_filename() &&
            !path.parent_path().empty() && path.parent_path().is_absolute();
    } catch (...) {
        return false;
    }
}

bool GetLoadedModulePath(HMODULE module,
                         std::filesystem::path& path) noexcept {
    try {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetModuleFileNameW(
                module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return false;
            }
            if (length < buffer.size()) {
                buffer.resize(length);
                path = std::filesystem::path(buffer);
                return IsAbsoluteFilePath(path);
            }
            if (buffer.size() >= 32768 / 2) {
                return false;
            }
            buffer.resize(buffer.size() * 2, L'\0');
        }
    } catch (...) {
        return false;
    }
}

bool GetFinalFilePath(const std::filesystem::path& input,
                      std::filesystem::path& output) noexcept {
    HANDLE file = CreateFileW(input.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool success = false;
    try {
        std::wstring buffer(512, L'\0');
        for (;;) {
            const DWORD length = GetFinalPathNameByHandleW(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (length == 0) {
                break;
            }
            if (length < buffer.size()) {
                buffer.resize(length);
                output = std::filesystem::path(buffer);
                success = IsAbsoluteFilePath(output);
                break;
            }
            if (length >= 32768) {
                break;
            }
            buffer.resize(static_cast<std::size_t>(length) + 1, L'\0');
        }
    } catch (...) {
        success = false;
    }
    CloseHandle(file);
    return success;
}

bool IsSafeDirectory(const std::filesystem::path& path) noexcept {
    if (path.empty() || !path.is_absolute()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsSafeRegularFile(const std::filesystem::path& path) noexcept {
    if (!IsAbsoluteFilePath(path)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool HasGogStorefrontMarker(const std::filesystem::path& game_root) noexcept {
    try {
        const std::filesystem::path pattern = game_root / L"goggame-*.info";
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool found = false;
        do {
            found = (entry.dwFileAttributes &
                     (FILE_ATTRIBUTE_DIRECTORY |
                      FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
        } while (!found && FindNextFileW(search, &entry));
        FindClose(search);
        return found;
    } catch (...) {
        return false;
    }
}

std::filesystem::path ResolveDisplaySettingsPath(
    const std::filesystem::path& game_root) noexcept {
    try {
        const std::filesystem::path gog =
            game_root / L"Save" / L"DisplaySettings.xml";
        const std::filesystem::path steam =
            game_root / L"data" / L"DisplaySettings.xml";
        if (IsSafeRegularFile(gog)) {
            return gog;
        }
        if (HasGogStorefrontMarker(game_root)) {
            return IsSafeRegularFile(gog) ? gog : std::filesystem::path{};
        }
        return IsSafeRegularFile(steam) ? steam : std::filesystem::path{};
    } catch (...) {
        return {};
    }
}

bool ResolvePaths(HMODULE module, LoadedPaths& paths) noexcept {
    std::filesystem::path module_reported;
    std::filesystem::path executable_reported;
    if (!GetLoadedModulePath(module, module_reported) ||
        !GetLoadedModulePath(nullptr, executable_reported) ||
        !GetFinalFilePath(module_reported, paths.module) ||
        !GetFinalFilePath(executable_reported, paths.executable) ||
        !PathNameEquals(paths.module, kModuleName) ||
        !PathNameEquals(paths.executable, kExecutableName)) {
        return false;
    }

    try {
        paths.game_root = paths.executable.parent_path();
        const std::filesystem::path module_root = paths.module.parent_path();
        if (!IsSafeDirectory(paths.game_root) ||
            !PathsEqual(paths.game_root, module_root)) {
            return false;
        }
        paths.config = module_root / kConfigName;
        paths.restore_journal = module_root / kRestoreJournalName;
        paths.display_settings = ResolveDisplaySettingsPath(paths.game_root);
    } catch (...) {
        return false;
    }
    return true;
}

ReadStatus ReadSmallFile(const std::filesystem::path& path,
                         std::size_t maximum_size,
                         std::string& text) noexcept {
    text.clear();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
                       GetLastError() == ERROR_PATH_NOT_FOUND
                   ? ReadStatus::Missing
                   : ReadStatus::Rejected;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return ReadStatus::Rejected;
    }

    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL |
                                  FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return ReadStatus::Rejected;
    }

    LARGE_INTEGER size{};
    bool success = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
        static_cast<unsigned long long>(size.QuadPart) <= maximum_size &&
        static_cast<unsigned long long>(size.QuadPart) <=
            (std::numeric_limits<DWORD>::max)();
    if (success) {
        try {
            text.resize(static_cast<std::size_t>(size.QuadPart));
        } catch (...) {
            success = false;
        }
    }
    if (success && !text.empty()) {
        DWORD bytes_read = 0;
        success = ReadFile(file,
                           text.data(),
                           static_cast<DWORD>(text.size()),
                           &bytes_read,
                           nullptr) && bytes_read == text.size();
    }
    CloseHandle(file);
    if (!success) {
        text.clear();
        return ReadStatus::Rejected;
    }
    return ReadStatus::Ok;
}

bool WriteAll(HANDLE file, std::string_view text) noexcept {
    std::size_t written_total = 0;
    while (written_total < text.size()) {
        const std::size_t remaining = text.size() - written_total;
        const DWORD request = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file,
                       text.data() + written_total,
                       request,
                       &written,
                       nullptr) || written == 0 || written > request) {
            return false;
        }
        written_total += written;
    }
    return true;
}

bool WriteFileAtomically(const std::filesystem::path& target,
                         std::string_view text) noexcept {
    try {
        if (!IsSafeDirectory(target.parent_path())) {
            return false;
        }
        const DWORD existing_attributes = GetFileAttributesW(target.c_str());
        if (existing_attributes != INVALID_FILE_ATTRIBUTES &&
            (existing_attributes & (FILE_ATTRIBUTE_DIRECTORY |
                                    FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            return false;
        }

        const unsigned long serial =
            g_atomic_write_serial.fetch_add(1, std::memory_order_relaxed);
        const std::wstring temporary_name =
            target.filename().wstring() + L".shenlong." +
            std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetCurrentThreadId()) + L"." +
            std::to_wstring(serial) + L".tmp";
        const std::filesystem::path temporary =
            target.parent_path() / temporary_name;
        HANDLE file = CreateFileW(temporary.c_str(),
                                  GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_WRITE_THROUGH,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        const bool wrote = WriteAll(file, text) && FlushFileBuffers(file);
        CloseHandle(file);
        if (!wrote || !MoveFileExW(temporary.c_str(),
                                   target.c_str(),
                                   MOVEFILE_REPLACE_EXISTING |
                                       MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool WriteSsaoQuality(const std::filesystem::path& path,
                      int quality,
                      std::optional<int>* previous = nullptr) noexcept {
    std::string xml;
    if (ReadSmallFile(path, kMaximumSettingsBytes, xml) != ReadStatus::Ok) {
        return false;
    }
    try {
        SsaoXmlEdit edit = EditSsaoXml(xml, quality);
        if (previous != nullptr) {
            *previous = edit.previous_value;
        }
        if (edit.status != SsaoXmlStatus::Ok) {
            return false;
        }
        return !edit.changed || WriteFileAtomically(path, edit.text);
    } catch (...) {
        return false;
    }
}

bool DeleteRestoreJournal(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }
    return DeleteFileW(path.c_str()) != FALSE;
}

bool RecoverRestoreJournal(const LoadedPaths& paths) noexcept {
    std::string journal;
    const ReadStatus status =
        ReadSmallFile(paths.restore_journal, 64, journal);
    if (status == ReadStatus::Missing) {
        return true;
    }
    if (status != ReadStatus::Ok || paths.display_settings.empty()) {
        Log(reshade::log::level::error,
            "[ShenLong-Native] pending AO restore journal is unreadable or "
            "DisplaySettings.xml is unavailable; native bridge disabled.");
        return false;
    }
    const std::optional<int> quality = ParseRestoreJournal(journal);
    if (!quality.has_value() ||
        !WriteSsaoQuality(paths.display_settings, *quality)) {
        Log(reshade::log::level::error,
            "[ShenLong-Native] pending AO restore could not be completed; "
            "journal retained and native bridge disabled.");
        return false;
    }
    if (!DeleteRestoreJournal(paths.restore_journal)) {
        Log(reshade::log::level::warning,
            "[ShenLong-Native] AO setting recovered to %d, but its idempotent "
            "restore journal could not be removed.",
            *quality);
    }
    Log(reshade::log::level::info,
        "[ShenLong-Native] recovered interrupted AO startup staging to %d.",
        *quality);
    return true;
}

bool ComputeSha256(const std::filesystem::path& path,
                   std::size_t& file_size,
                   std::array<std::uint8_t, 32>& hash) noexcept {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL |
                                  FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            (std::numeric_limits<std::size_t>::max)()) {
        CloseHandle(file);
        return false;
    }
    file_size = static_cast<std::size_t>(size.QuadPart);
    const bool supported_size = std::any_of(
        kExecutableProfiles.begin(), kExecutableProfiles.end(),
        [file_size](const ExecutableProfile& profile) noexcept {
            return profile.file_size == file_size;
        });
    if (!supported_size) {
        CloseHandle(file);
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH crypt_hash = 0;
    bool success = CryptAcquireContextW(
        &provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &crypt_hash);
    std::vector<std::uint8_t> buffer;
    if (success) {
        try {
            buffer.resize(64u * 1024u);
        } catch (...) {
            success = false;
        }
    }
    while (success) {
        DWORD bytes_read = 0;
        if (!ReadFile(file,
                      buffer.data(),
                      static_cast<DWORD>(buffer.size()),
                      &bytes_read,
                      nullptr)) {
            success = false;
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        if (!CryptHashData(crypt_hash, buffer.data(), bytes_read, 0)) {
            success = false;
        }
    }
    if (success) {
        DWORD hash_size = static_cast<DWORD>(hash.size());
        success = CryptGetHashParam(
                      crypt_hash, HP_HASHVAL, hash.data(), &hash_size, 0) &&
            hash_size == hash.size();
    }
    if (crypt_hash != 0) {
        CryptDestroyHash(crypt_hash);
    }
    if (provider != 0) {
        CryptReleaseContext(provider, 0);
    }
    CloseHandle(file);
    return success;
}

bool ReadLoadedPeMetadata(HMODULE executable,
                          std::uint32_t& timestamp,
                          std::uint32_t& size_of_image) noexcept {
    if (executable == nullptr) {
        return false;
    }
    __try {
        const auto* base = reinterpret_cast<const std::uint8_t*>(executable);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x100000) {
            return false;
        }
        const std::uintptr_t base_address =
            reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t nt_offset =
            static_cast<std::uintptr_t>(dos->e_lfanew);
        if (base_address >
            (std::numeric_limits<std::uintptr_t>::max)() - nt_offset) {
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base_address + nt_offset);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.SizeOfImage == 0) {
            return false;
        }
        timestamp = nt->FileHeader.TimeDateStamp;
        size_of_image = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MatchesSignature(const void* address,
                      const std::uint8_t* signature,
                      std::size_t size) noexcept {
    if (address == nullptr || signature == nullptr || size == 0) {
        return false;
    }
    __try {
        return std::memcmp(address, signature, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadInt(std::uintptr_t address, int& value) noexcept {
    value = 0;
    if (address == 0) {
        return false;
    }
    __try {
        value = *reinterpret_cast<const int*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

bool SafeWriteInt(std::uintptr_t address, int value) noexcept {
    if (address == 0) {
        return false;
    }
    __try {
        *reinterpret_cast<int*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const ExecutableProfile* VerifyExecutableIdentity(
    const LoadedPaths& paths,
    ExecutableIdentityObservation& observation) noexcept {
    observation = {};
    HMODULE executable = GetModuleHandleW(nullptr);
    observation.hash_read = ComputeSha256(
        paths.executable, observation.file_size, observation.sha256);
    observation.pe_read = ReadLoadedPeMetadata(
        executable, observation.timestamp, observation.size_of_image);
    if (!observation.hash_read || !observation.pe_read) {
        return nullptr;
    }
    return FindExecutableProfile(
        observation.file_size,
        observation.timestamp,
        observation.size_of_image,
        observation.sha256);
}

std::array<char, 65> FormatSha256(
    const std::array<std::uint8_t, 32>& hash) noexcept {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::array<char, 65> text{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        text[index * 2] = kHex[hash[index] >> 4];
        text[index * 2 + 1] = kHex[hash[index] & 0x0F];
    }
    return text;
}

Settings LoadSettings(const std::filesystem::path& config_path) noexcept {
    Settings settings;
    try {
        const std::wstring path = config_path.wstring();
        namespace ini = spatch::graphics::ini;
        settings.config_version = ini::ParseInt(
            ini::ReadValue(path, L"ShenLong", L"ConfigVersion"), 0);
        settings.enabled = ini::ReadBool(
            path, ini::kMasterEnabledKeys, false);
        const std::optional<std::wstring> raw_mode = ini::ReadValue(
            path, L"AmbientOcclusion", L"AmbientOcclusion");
        settings.ao_mode = raw_mode.has_value()
            ? ParseAoMode(*raw_mode, AoMode::Original)
            : AoMode::Original;
        settings.original_ao_quality = ValidateOriginalAoQuality(
            ini::ReadInt(path,
                         std::array{ini::Key{L"AmbientOcclusion",
                                             L"OriginalAOQuality"}},
                         -1));
        settings.stock_hair_blur = ini::ReadBool(
            path,
            std::array{ini::Key{L"SubsurfaceScattering",
                                L"StockHairBlur"}},
            false);
    } catch (...) {
        settings = {};
    }
    return settings;
}

FILETIME RelativeDueTime(DWORD milliseconds) noexcept {
    const LONGLONG value =
        -static_cast<LONGLONG>(milliseconds == 0 ? 1 : milliseconds) *
        10000;
    ULARGE_INTEGER encoded{};
    encoded.QuadPart = static_cast<ULONGLONG>(value);
    return FILETIME{encoded.LowPart, encoded.HighPart};
}

bool RestorePendingAoSetting() noexcept {
    std::lock_guard lock(g_restore_mutex);
    if (!g_restore_quality.has_value()) {
        return true;
    }
    const int quality = *g_restore_quality;
    if (!WriteSsaoQuality(g_restore_display_path, quality)) {
        return false;
    }
    if (!DeleteRestoreJournal(g_restore_journal_path)) {
        Log(reshade::log::level::warning,
            "[ShenLong-Native] AO startup staging restored to %d, but the "
            "idempotent journal could not be removed.",
            quality);
    }
    g_restore_display_path.clear();
    g_restore_journal_path.clear();
    g_restore_quality.reset();
    g_restore_retry_count = 0;
    if (g_restore_timer != nullptr) {
        SetThreadpoolTimer(g_restore_timer, nullptr, 0, 0);
    }
    Log(reshade::log::level::info,
        "[ShenLong-Native] restored saved Original AO quality=%d after "
        "scheduler initialization.",
        quality);
    return true;
}

void CALLBACK RestoreTimerCallback(PTP_CALLBACK_INSTANCE,
                                   void*,
                                   PTP_TIMER) noexcept {
    if (RestorePendingAoSetting()) {
        return;
    }
    bool scheduled = false;
    {
        std::lock_guard lock(g_restore_mutex);
        if (g_restore_quality.has_value() && g_restore_timer != nullptr &&
            g_restore_retry_count < kRestoreMaximumRetries) {
            ++g_restore_retry_count;
            FILETIME due = RelativeDueTime(kRestoreRetryDelayMilliseconds);
            SetThreadpoolTimer(g_restore_timer, &due, 0, 0);
            scheduled = true;
        }
    }
    if (!scheduled) {
        Log(reshade::log::level::error,
            "[ShenLong-Native] AO restore retries exhausted; crash-safe "
            "journal retained for the next launch.");
    }
}

void CALLBACK RestoreImmediateCallback(PTP_CALLBACK_INSTANCE,
                                       void*) noexcept {
    (void)RestorePendingAoSetting();
}

bool ArmRestore(const LoadedPaths& paths, int quality) noexcept {
    std::lock_guard lock(g_restore_mutex);
    if (g_restore_timer == nullptr) {
        g_restore_timer =
            CreateThreadpoolTimer(&RestoreTimerCallback, nullptr, nullptr);
    }
    if (g_restore_timer == nullptr) {
        return false;
    }
    g_restore_display_path = paths.display_settings;
    g_restore_journal_path = paths.restore_journal;
    g_restore_quality = quality;
    g_restore_retry_count = 0;
    FILETIME due = RelativeDueTime(kRestoreFallbackDelayMilliseconds);
    SetThreadpoolTimer(g_restore_timer, &due, 0, 0);
    return true;
}

void RequestImmediateRestore() noexcept {
    bool expected = false;
    if (!g_ao_restore_requested.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!TrySubmitThreadpoolCallback(
            &RestoreImmediateCallback, nullptr, nullptr)) {
        Log(reshade::log::level::warning,
            "[ShenLong-Native] immediate AO restore callback unavailable; "
            "bounded fallback timer remains armed.");
    }
}

class AcceptedCall {
public:
    AcceptedCall() noexcept {
        if (!g_behavior_accepting.load(std::memory_order_acquire)) {
            return;
        }
        g_accepted_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!g_behavior_accepting.load(std::memory_order_acquire)) {
            g_accepted_calls.fetch_sub(1, std::memory_order_release);
            return;
        }
        accepted_ = true;
    }

    AcceptedCall(const AcceptedCall&) = delete;
    AcceptedCall& operator=(const AcceptedCall&) = delete;

    ~AcceptedCall() {
        if (accepted_) {
            g_accepted_calls.fetch_sub(1, std::memory_order_release);
        }
    }

    bool accepted() const noexcept { return accepted_; }

private:
    bool accepted_ = false;
};

void DetourAmbientOcclusionStage(char* ao_state,
                                 std::uintptr_t render_context,
                                 long long* command_list) {
    AcceptedCall call;
    if (call.accepted() &&
        g_custom_ao_enabled.load(std::memory_order_acquire)) {
        RequestImmediateRestore();
        int live_quality = 0;
        constexpr std::uintptr_t kSsaoOffset = 0x15C;
        if (render_context != 0) {
            const bool read =
                SafeReadInt(render_context + kSsaoOffset, live_quality);
            const bool selected = read &&
                (live_quality == 1 ||
                 SafeWriteInt(render_context + kSsaoOffset, 1));
            if (selected) {
                if (!g_ao_live_scheduler_logged.exchange(
                        true, std::memory_order_acq_rel)) {
                    Log(reshade::log::level::info,
                        "[ShenLong-Native] custom AO observed the verified "
                        "high-quality live scheduler path (before=%d).",
                        live_quality);
                }
            } else if (!g_ao_live_scheduler_warning_logged.exchange(
                           true, std::memory_order_acq_rel)) {
                Log(reshade::log::level::warning,
                    "[ShenLong-Native] custom AO could not select the live "
                    "high-quality scheduler; native AO behavior retained.");
            }
        }
    }
    const AmbientOcclusionStageFn original = g_ao_stage_original;
    if (original != nullptr) {
        original(ao_state, render_context, command_list);
    }
}

void DetourHairBlurSubmit(std::uintptr_t hair_skin_stage,
                          std::uintptr_t render_context,
                          std::uintptr_t render_command,
                          std::uintptr_t source_surface,
                          std::uintptr_t material_state,
                          std::uintptr_t view_state) {
    AcceptedCall call;
    if (call.accepted() &&
        g_hair_blur_suppression_enabled.load(std::memory_order_acquire)) {
        if (!g_hair_blur_suppression_logged.exchange(
                true, std::memory_order_acq_rel)) {
            Log(reshade::log::level::info,
                "[ShenLong-Native] StockHairBlur=0 skipped the exact named "
                "HairBlur submission.");
        }
        return;
    }
    const HairBlurSubmitFn original = g_hair_blur_submit_original;
    if (original != nullptr) {
        original(hair_skin_stage,
                 render_context,
                 render_command,
                 source_surface,
                 material_state,
                 view_state);
    }
}

bool ValidateTarget(spatch::hook_guard::Guard& guard,
                    const void* target,
                    const std::array<std::uint8_t, 32>& signature,
                    const char* name) noexcept {
    const hook_guard::Result result = guard.Verify(target, signature.size());
    if (!result.verified() ||
        !MatchesSignature(target, signature.data(), signature.size())) {
        Log(reshade::log::level::error,
            "[ShenLong-Native] hook target rejected name=%s status=%s "
            "pe_status=%s rva=0x%X mismatch=%zu.",
            name,
            hook_guard::StatusName(result.status),
            hook_guard::PeStatusName(result.pe_status),
            result.target_rva,
            result.mismatch_offset);
        return false;
    }
    return true;
}

bool ResolveTarget(HMODULE executable,
                   const ExecutableProfile& profile,
                   std::uint32_t rva,
                   void*& target) noexcept {
    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(executable);
    if (base == 0 || rva >= profile.size_of_image ||
        base > (std::numeric_limits<std::uintptr_t>::max)() - rva) {
        target = nullptr;
        return false;
    }
    target = reinterpret_cast<void*>(base + rva);
    return true;
}

bool StageCustomAo(const LoadedPaths& paths,
                   int original_quality) noexcept {
    if (paths.display_settings.empty()) {
        return false;
    }

    std::string xml;
    if (ReadSmallFile(paths.display_settings,
                      kMaximumSettingsBytes,
                      xml) != ReadStatus::Ok) {
        return false;
    }
    const SsaoXmlInspection inspection = InspectSsaoXml(xml);
    if (inspection.status != SsaoXmlStatus::Ok) {
        return false;
    }
    const int restore_quality = original_quality >= 0
        ? original_quality
        : (inspection.value.has_value() &&
                   (*inspection.value == 0 || *inspection.value == 1)
               ? *inspection.value
               : -1);
    if (restore_quality < 0) {
        return false;
    }

    if (inspection.value.has_value() && *inspection.value == 1 &&
        restore_quality == 1) {
        return true;
    }
    if (!ArmRestore(paths, restore_quality)) {
        return false;
    }
    if (!WriteFileAtomically(paths.restore_journal,
                             restore_quality == 0 ? "0\n" : "1\n") ||
        !WriteSsaoQuality(paths.display_settings, 1)) {
        (void)RestorePendingAoSetting();
        return false;
    }
    Log(reshade::log::level::info,
        "[ShenLong-Native] staged SSAO=High for custom-AO scheduler "
        "initialization; saved quality=%d and crash-safe restore armed.",
        restore_quality);
    return true;
}

bool InstallRequestedHooks(const ExecutableProfile& profile,
                           const Settings& settings,
                           const LoadedPaths& paths) noexcept {
    const bool need_ao = UsesCustomAo(settings.ao_mode);
    const bool need_hair = !settings.stock_hair_blur;
    if (!need_ao && !need_hair) {
        return true;
    }
    if (g_stop_requested.load(std::memory_order_acquire)) {
        return false;
    }

    HMODULE executable = GetModuleHandleW(nullptr);
    void* ao_target = nullptr;
    void* hair_target = nullptr;
    if ((need_ao && !ResolveTarget(executable,
                                   profile,
                                   profile.ao_stage_rva,
                                   ao_target)) ||
        (need_hair && !ResolveTarget(executable,
                                     profile,
                                     profile.hair_blur_submit_rva,
                                     hair_target))) {
        return false;
    }

    hook_guard::Guard guard;
    hook_guard::Result initialization_result;
    if (!guard.Initialize(executable, &initialization_result)) {
        Log(reshade::log::level::error,
            "[ShenLong-Native] pristine executable guard rejected hooks "
            "status=%s pe_status=%s.",
            hook_guard::StatusName(initialization_result.status),
            hook_guard::PeStatusName(initialization_result.pe_status));
        return false;
    }
    if ((need_ao && !ValidateTarget(guard,
                                    ao_target,
                                    profile.ao_stage_signature,
                                    "ao_stage")) ||
        (need_hair && !ValidateTarget(guard,
                                      hair_target,
                                      profile.hair_blur_submit_signature,
                                      "hair_blur_submit"))) {
        return false;
    }

    bool installed = true;
    if (need_ao) {
        const MH_STATUS status = MH_CreateHook(
            ao_target,
            reinterpret_cast<void*>(&DetourAmbientOcclusionStage),
            reinterpret_cast<void**>(&g_ao_stage_original));
        installed = status == MH_OK;
        Log(installed ? reshade::log::level::info
                      : reshade::log::level::error,
            "[ShenLong-Native] AO-stage hook profile=%s rva=0x%X "
            "installed=%d minhook=%d.",
            profile.id,
            profile.ao_stage_rva,
            installed ? 1 : 0,
            static_cast<int>(status));
    }
    if (need_hair && installed) {
        const MH_STATUS status = MH_CreateHook(
            hair_target,
            reinterpret_cast<void*>(&DetourHairBlurSubmit),
            reinterpret_cast<void**>(&g_hair_blur_submit_original));
        installed = status == MH_OK;
        Log(installed ? reshade::log::level::info
                      : reshade::log::level::error,
            "[ShenLong-Native] HairBlur hook profile=%s rva=0x%X "
            "installed=%d minhook=%d.",
            profile.id,
            profile.hair_blur_submit_rva,
            installed ? 1 : 0,
            static_cast<int>(status));
    }
    if (!installed) {
        Log(reshade::log::level::warning,
            "[ShenLong-Native] native hook transaction rejected; any "
            "created detour is retained as transparent pass-through.");
        return false;
    }

    if (g_stop_requested.load(std::memory_order_acquire) ||
        (need_ao &&
         !StageCustomAo(paths, settings.original_ao_quality))) {
        Log(reshade::log::level::error,
            "[ShenLong-Native] custom-AO scheduler staging failed; native "
            "hook transaction remains transparent.");
        return false;
    }

    if (g_stop_requested.load(std::memory_order_acquire)) {
        (void)RestorePendingAoSetting();
        return false;
    }

    // MH_CreateHook in the pinned SDmodding fork installs the detours here,
    // but their shared acceptance gate deliberately remains closed. The
    // pre-device bootstrap commits behavior only after every renderer event
    // callback has registered successfully.
    return true;
}

bool PinVerifiedModule() noexcept {
    HMODULE pinned = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(g_module),
                          &pinned)) {
        return true;
    }
    Log(reshade::log::level::error,
        "[ShenLong-Native] verified module pin failed error=%lu; graphics "
        "components and native hooks disabled.",
        GetLastError());
    return false;
}

DWORD BootstrapWork() noexcept {
    try {
        LoadedPaths paths;
        if (g_stop_requested.load(std::memory_order_acquire) ||
            !ResolvePaths(g_module, paths)) {
            Log(reshade::log::level::error,
                "[ShenLong-Native] absolute module/game path "
                "validation failed; native behavior retained.");
            return 0;
        }

        // Recover a previously armed AO transaction before new configuration
        // or executable authorization. A crash can leave the stock SSAO value
        // staged even if ShenLong.ini is later removed or the game updates to
        // an unsupported executable; the same hardened module/game paths and
        // journal still provide enough authority to restore that value.
        if (!RecoverRestoreJournal(paths) ||
            g_stop_requested.load(std::memory_order_acquire)) {
            return 0;
        }
        if (!IsSafeRegularFile(paths.config)) {
            Log(reshade::log::level::error,
                "[ShenLong-Native] ShenLong.ini is missing or unsafe; "
                "graphics components and native hooks disabled.");
            return 0;
        }

        const Settings settings = LoadSettings(paths.config);
        Log(reshade::log::level::info,
            "[ShenLong-Native] config version=%d enabled=%d ao=%s "
            "original_ao_quality=%d stock_hair_blur=%d.",
            settings.config_version,
            settings.enabled ? 1 : 0,
            settings.ao_mode == AoMode::Original
                ? "Original"
                : (settings.ao_mode == AoMode::Sdao ? "SDAO" : "GTAOLite"),
            settings.original_ao_quality,
            settings.stock_hair_blur ? 1 : 0);
        if (settings.config_version != kShenLongConfigVersion) {
            Log(reshade::log::level::error,
                "[ShenLong-Native] unsupported ShenLong.ini ConfigVersion=%d "
                "(expected %d); graphics components and native hooks "
                "disabled.",
                settings.config_version,
                kShenLongConfigVersion);
            return 0;
        }
        if (!settings.enabled ||
            g_stop_requested.load(std::memory_order_acquire)) {
            return 0;
        }

        ExecutableIdentityObservation observation;
        const ExecutableProfile* profile =
            VerifyExecutableIdentity(paths, observation);
        if (profile == nullptr) {
            const std::array<char, 65> hash =
                FormatSha256(observation.sha256);
            Log(reshade::log::level::error,
                "[ShenLong-Native] executable identity rejected "
                "hash_read=%d pe_read=%d file_size=%zu timestamp=0x%08X "
                "image_size=0x%08X sha256=%s; verified graphics components "
                "and fixed-RVA hooks disabled.",
                observation.hash_read ? 1 : 0,
                observation.pe_read ? 1 : 0,
                observation.file_size,
                observation.timestamp,
                observation.size_of_image,
                observation.hash_read ? hash.data() : "unavailable");
            return 0;
        }
        Log(reshade::log::level::info,
            "[ShenLong-Native] executable identity verified profile=%s "
            "file_size=%zu timestamp=0x%08X image_size=0x%08X "
            "ao_rva=0x%X hair_rva=0x%X.",
            profile->id,
            profile->file_size,
            profile->timestamp,
            profile->size_of_image,
            profile->ao_stage_rva,
            profile->hair_blur_submit_rva);

        if (!PinVerifiedModule()) {
            return 0;
        }

        if (settings.ao_mode == AoMode::Original &&
            settings.original_ao_quality >= 0) {
            if (paths.display_settings.empty() ||
                !WriteSsaoQuality(paths.display_settings,
                                  settings.original_ao_quality)) {
                Log(reshade::log::level::error,
                    "[ShenLong-Native] OriginalAOQuality=%d could not be "
                    "persisted; existing game setting retained.",
                    settings.original_ao_quality);
            } else {
                Log(reshade::log::level::info,
                    "[ShenLong-Native] persisted stock Original AO quality=%d.",
                    settings.original_ao_quality);
            }
        }

        if (!InstallRequestedHooks(*profile, settings, paths)) {
            Log(reshade::log::level::error,
                "[ShenLong-Native] requested native transaction failed; "
                "graphics component registration disabled.");
            return 0;
        }

        const VerifiedTargetCallback on_verified = g_on_verified;
        if (on_verified == nullptr ||
            !on_verified(g_module, *profile) ||
            g_stop_requested.load(std::memory_order_acquire)) {
            g_behavior_accepting.store(false, std::memory_order_release);
            g_custom_ao_enabled.store(false, std::memory_order_release);
            g_hair_blur_suppression_enabled.store(
                false, std::memory_order_release);
            (void)RestorePendingAoSetting();
            Log(reshade::log::level::error,
                "[ShenLong-Native] verified graphics component registration "
                "failed; profile unpublished and native hooks transparent.");
            return 0;
        }
        g_verified_profile.store(profile, std::memory_order_release);
        const bool custom_ao = UsesCustomAo(settings.ao_mode);
        const bool suppress_hair_blur = !settings.stock_hair_blur;
        g_custom_ao_enabled.store(custom_ao, std::memory_order_release);
        g_hair_blur_suppression_enabled.store(
            suppress_hair_blur, std::memory_order_release);
        g_behavior_accepting.store(
            custom_ao || suppress_hair_blur, std::memory_order_release);
        Log(reshade::log::level::info,
            "[ShenLong-Native] verified pre-device transaction committed "
            "profile=%s custom_ao=%d stock_hair_blur=%d.",
            profile->id,
            custom_ao ? 1 : 0,
            settings.stock_hair_blur ? 1 : 0);
    } catch (...) {
        g_verified_profile.store(nullptr, std::memory_order_release);
        g_behavior_accepting.store(false, std::memory_order_release);
        g_custom_ao_enabled.store(false, std::memory_order_release);
        g_hair_blur_suppression_enabled.store(
            false, std::memory_order_release);
        (void)RestorePendingAoSetting();
        Log(reshade::log::level::error,
            "[ShenLong-Native] bootstrap exception; native behavior retained.");
    }
    return 0;
}

bool OnCreateDevice(
    reshade::api::device_api api,
    std::uint32_t&) noexcept {
    if (api != reshade::api::device_api::d3d11) {
        return false;
    }
    try {
        // ReShade dispatches create_device before the real D3D11CreateDevice
        // call. call_once also serializes concurrent device-creation attempts,
        // so component registration is complete or rejected before any
        // init_device event can run. Always return false: ShenLong never
        // overrides the requested API version.
        std::call_once(g_bootstrap_once, []() noexcept {
            (void)BootstrapWork();
        });
    } catch (...) {
        g_verified_profile.store(nullptr, std::memory_order_release);
        g_behavior_accepting.store(false, std::memory_order_release);
        g_custom_ao_enabled.store(false, std::memory_order_release);
        g_hair_blur_suppression_enabled.store(
            false, std::memory_order_release);
        Log(reshade::log::level::error,
            "[ShenLong-Native] pre-device once gate failed; native rendering "
            "retained.");
    }
    return false;
}

}  // namespace

bool Attach(HMODULE module, VerifiedTargetCallback on_verified) noexcept {
    if (module == nullptr || on_verified == nullptr ||
        g_started.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    g_module = module;
    g_on_verified = on_verified;
    reshade::register_event<reshade::addon_event::create_device>(
        OnCreateDevice);
    return true;
}

const ExecutableProfile* GetVerifiedExecutableProfile() noexcept {
    return g_verified_profile.load(std::memory_order_acquire);
}

void Detach(bool process_terminating) noexcept {
    g_stop_requested.store(true, std::memory_order_release);
    g_verified_profile.store(nullptr, std::memory_order_release);
    g_behavior_accepting.store(false, std::memory_order_release);
    g_custom_ao_enabled.store(false, std::memory_order_release);
    g_hair_blur_suppression_enabled.store(false, std::memory_order_release);
    if (process_terminating) {
        return;
    }

    reshade::unregister_event<reshade::addon_event::create_device>(
        OnCreateDevice);
    (void)RestorePendingAoSetting();
    const ULONGLONG deadline = GetTickCount64() + kDetachDrainMilliseconds;
    while (g_accepted_calls.load(std::memory_order_acquire) != 0 &&
           GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (g_accepted_calls.load(std::memory_order_acquire) != 0) {
        Log(reshade::log::level::warning,
            "[ShenLong-Native] detach drain timed out; pinned transparent "
            "detours and restore journal retained.");
    }
}

}  // namespace spatch::graphics::native
