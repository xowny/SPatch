#include "VersionGuard.h"

#include "BuildInfo.h"
#include "Logger.h"

#include <Windows.h>
#include <wincrypt.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace spatch {
namespace {

std::wstring GetProcessImagePath() {
    try {
        std::wstring buffer(MAX_PATH, L'\0');

        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                    static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return {};
            }

            if (length < buffer.size()) {
                buffer.resize(length);
                return buffer;
            }
            if (buffer.size() >= 32768 / 2) {
                return {};
            }
            buffer.resize(buffer.size() * 2);
        }
    } catch (...) {
        return {};
    }
}

bool ReadPeMetadata(const std::filesystem::path& path,
                    std::size_t& file_size,
                    unsigned long& timestamp,
                    unsigned long& size_of_image) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    stream.seekg(0, std::ios::end);
    const std::streampos end_position = stream.tellg();
    if (end_position < 0) {
        return false;
    }
    const auto end_offset = static_cast<unsigned long long>(end_position);
    if (end_offset > static_cast<unsigned long long>(
                         (std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    file_size = static_cast<std::size_t>(end_offset);
    stream.seekg(0, std::ios::beg);

    IMAGE_DOS_HEADER dos_header{};
    stream.read(reinterpret_cast<char*>(&dos_header), sizeof(dos_header));
    if (stream.gcount() != sizeof(dos_header) || dos_header.e_magic != IMAGE_DOS_SIGNATURE ||
        dos_header.e_lfanew <= 0) {
        return false;
    }

    const auto nt_offset = static_cast<std::size_t>(dos_header.e_lfanew);
    constexpr std::size_t kMinimumNtBytes =
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64);
    if (nt_offset > file_size || file_size - nt_offset < kMinimumNtBytes) {
        return false;
    }

    stream.clear();
    stream.seekg(dos_header.e_lfanew, std::ios::beg);
    DWORD signature = 0;
    IMAGE_FILE_HEADER file_header{};
    IMAGE_OPTIONAL_HEADER64 optional_header{};
    stream.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    stream.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
    if (!stream || signature != IMAGE_NT_SIGNATURE ||
        file_header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        file_header.SizeOfOptionalHeader < sizeof(optional_header)) {
        return false;
    }
    stream.read(reinterpret_cast<char*>(&optional_header), sizeof(optional_header));
    if (!stream || optional_header.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        optional_header.SizeOfImage == 0) {
        return false;
    }

    timestamp = file_header.TimeDateStamp;
    size_of_image = optional_header.SizeOfImage;
    return true;
}

bool ComputeSha256Internal(const std::filesystem::path& path,
                           std::array<unsigned char, 32>& hash_out,
                           ReadFileCallback read_file_callback) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool success = false;

    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        CloseHandle(file);
        return false;
    }

    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        CloseHandle(file);
        return false;
    }

    // Hashing runs during bootstrap, where keeping a 64 KiB object off the
    // worker's stack leaves substantially more headroom for loader/runtime
    // frames and satisfies the native stack-use analyzer.
    DWORD bytes_read = 0;
    BOOL read_ok = FALSE;
    std::vector<std::byte> buffer;
    try {
        buffer.resize(1 << 16);
    } catch (...) {
        // The crypto/file handles are still live here.  Route allocation
        // failure through the common cleanup path instead of leaking them
        // until process exit (the old direct-sized construction could throw
        // before cleanup was reached).
        goto cleanup;
    }
    while ((read_ok = read_file_callback(file,
                                         buffer.data(),
                                         static_cast<DWORD>(buffer.size()),
                                         &bytes_read,
                                         nullptr)) &&
           bytes_read != 0) {
        if (bytes_read > buffer.size()) {
            // A ReadFile-compatible callback must not hand the hash routine a
            // count larger than the buffer it supplied.  Reject malformed
            // test/loader callbacks rather than hashing past the allocation.
            goto cleanup;
        }
        if (!CryptHashData(hash,
                           reinterpret_cast<const BYTE*>(buffer.data()),
                           bytes_read,
                           0)) {
            goto cleanup;
        }
    }

    if (!read_ok) {
        goto cleanup;
    }

    {
        DWORD hash_size = static_cast<DWORD>(hash_out.size());
        if (!CryptGetHashParam(hash, HP_HASHVAL, hash_out.data(), &hash_size, 0) ||
            hash_size != hash_out.size()) {
            goto cleanup;
        }
    }

    success = true;

cleanup:
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return success;
}

std::string HexString(const std::array<unsigned char, 32>& value) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (const unsigned char byte : value) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

std::string Narrow(const std::wstring& text) {
    return log::ToUtf8(text);
}

}  // namespace

std::string IdentifyBuildFromPeMetadata(std::size_t file_size,
                                        unsigned long time_date_stamp,
                                        unsigned long size_of_image) {
    if (file_size != build_info::kKnownFileSize ||
        size_of_image != build_info::kKnownSizeOfImage) {
        return "unknown";
    }

    if (time_date_stamp == build_info::kLegacyTimeDateStamp) {
        return "legacy_researched";
    }

    if (time_date_stamp == build_info::kLatestSteamTimeDateStamp) {
        return "latest_steam";
    }

    return "unknown";
}

bool ComputeSha256FromPath(const std::filesystem::path& path,
                           std::array<unsigned char, 32>& hash_out) {
    return ComputeSha256Internal(path, hash_out, &::ReadFile);
}

bool ComputeSha256FromPathWithReadCallback(const std::filesystem::path& path,
                                           std::array<unsigned char, 32>& hash_out,
                                           ReadFileCallback read_file_callback) {
    if (read_file_callback == nullptr) {
        return false;
    }
    return ComputeSha256Internal(path, hash_out, read_file_callback);
}

bool ShouldUseSafeCompatibilityMode(const BuildCheckResult& build, bool allow_unverified_build) {
    return !build.hook_layout_supported && (build.supported || allow_unverified_build);
}

BuildCheckResult InspectLoadedGame() {
    return InspectGameAtPath(GetProcessImagePath());
}

BuildCheckResult InspectGameAtPath(const std::filesystem::path& path) {
    return InspectGameAtPathWithHashCallback(path, &ComputeSha256FromPath);
}

BuildCheckResult InspectGameAtPathWithHashCallback(const std::filesystem::path& path,
                                                   ComputeSha256PathCallback hash_callback) {
    BuildCheckResult result;
    result.exe_path = path;

    const bool pe_ok = ReadPeMetadata(
        result.exe_path, result.file_size, result.time_date_stamp, result.size_of_image);
    bool legacy_match = false;
    bool latest_steam_match = false;
    std::string identity_path = "unknown";

    if (pe_ok) {
        const std::string metadata_build_id = IdentifyBuildFromPeMetadata(
            result.file_size, result.time_date_stamp, result.size_of_image);
        const bool metadata_match = metadata_build_id != "unknown";

        // Metadata is useful for diagnostics, but it is not sufficient to
        // authorize fixed-RVA hooks: a modified executable can retain the
        // same size/timestamp/image size.  Always prefer a SHA-256 identity
        // check when a callback is available.
        result.hash_computed =
            hash_callback != nullptr && hash_callback(result.exe_path, result.sha256);
        if (result.hash_computed) {
            identity_path = "sha256";
            legacy_match = result.sha256 == build_info::kLegacySha256;
            latest_steam_match = result.sha256 == build_info::kLatestSteamSha256;
            if (legacy_match) {
                result.build_id = "legacy_researched";
            } else if (latest_steam_match) {
                result.build_id = "latest_steam";
            } else {
                result.build_id = "unknown";
            }
        } else if (metadata_match) {
            // Keep the metadata identity visible in the log, but leave the
            // hook-layout flag false so bootstrap enters safe compatibility
            // mode instead of applying unverified addresses.
            identity_path = "metadata_unverified";
            result.build_id = metadata_build_id;
            legacy_match = metadata_build_id == "legacy_researched";
            latest_steam_match = metadata_build_id == "latest_steam";
        }

        if (legacy_match || latest_steam_match) {
            result.version_text = std::wstring(build_info::kSupportedVersionText);
            result.version_major = build_info::kSupportedVersionMajor;
            result.version_minor = build_info::kSupportedVersionMinor;
            result.version_build = build_info::kSupportedVersionBuild;
            result.version_private = build_info::kSupportedVersionPrivate;
        }
    }

    if (result.build_id.empty()) {
        result.build_id = "unknown";
    }

    result.supported = pe_ok && (legacy_match || latest_steam_match);
    result.hook_layout_supported = result.hash_computed && (legacy_match || latest_steam_match);

    const char* hook_layout_text = "safe_mode";
    // Metadata-only recognition is useful for diagnostics but never proves a
    // fixed-RVA layout. Keep the summary aligned with the actual policy so an
    // end user does not read "legacy_researched" while bootstrap deliberately
    // disabled every address-based hook.
    if (result.hook_layout_supported) {
        hook_layout_text = legacy_match ? "legacy_researched" : "latest_steam";
    }

    std::ostringstream summary;
    summary << "exe=" << Narrow(result.exe_path);
    if (!result.version_text.empty()) {
        summary << " version=" << Narrow(result.version_text);
    } else {
        summary << " version=skipped";
    }
    summary << " file_size=" << result.file_size
            << " timestamp=0x" << std::hex << std::uppercase << result.time_date_stamp
            << " size_of_image=0x" << result.size_of_image << std::dec;
    if (result.hash_computed) {
        summary << " sha256=" << HexString(result.sha256);
    } else {
        summary << " sha256=skipped";
    }
    summary << " identity_path=" << identity_path
            << " supported=" << (result.supported ? "yes" : "no")
            << " hook_layout=" << hook_layout_text
            << " build_id=" << result.build_id;
    result.summary = summary.str();

    return result;
}

}  // namespace spatch
