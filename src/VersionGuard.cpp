#include "VersionGuard.h"

#include "BuildInfo.h"
#include "Logger.h"

#include <Windows.h>
#include <wincrypt.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace spatch {
namespace {

std::wstring GetProcessImagePath() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }

        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }

        buffer.resize(buffer.size() * 2);
    }
}

bool ReadFileVersion(const std::filesystem::path& path,
                     std::wstring& version_text,
                     unsigned short& version_major,
                     unsigned short& version_minor,
                     unsigned short& version_build,
                     unsigned short& version_private) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return false;
    }

    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) {
        return false;
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        info == nullptr) {
        return false;
    }

    version_major = static_cast<unsigned short>(HIWORD(info->dwFileVersionMS));
    version_minor = static_cast<unsigned short>(LOWORD(info->dwFileVersionMS));
    version_build = static_cast<unsigned short>(HIWORD(info->dwFileVersionLS));
    version_private = static_cast<unsigned short>(LOWORD(info->dwFileVersionLS));

    std::wostringstream stream;
    stream << version_major << L'.' << version_minor << L'.' << version_build << L'.'
           << version_private;
    version_text = stream.str();
    return true;
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
    file_size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);

    std::vector<char> header(4096);
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (stream.gcount() < static_cast<std::streamsize>(sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }

    const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(header.data());
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE ||
        dos_header->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos_header->e_lfanew + sizeof(IMAGE_NT_HEADERS64)) > header.size()) {
        return false;
    }

    const auto* nt_headers =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(header.data() + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    timestamp = nt_headers->FileHeader.TimeDateStamp;
    size_of_image = nt_headers->OptionalHeader.SizeOfImage;
    return true;
}

bool ComputeSha256(const std::filesystem::path& path, std::array<unsigned char, 32>& hash_out) {
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

    std::array<std::byte, 1 << 16> buffer{};
    DWORD bytes_read = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) &&
           bytes_read != 0) {
        if (!CryptHashData(hash,
                           reinterpret_cast<const BYTE*>(buffer.data()),
                           bytes_read,
                           0)) {
            goto cleanup;
        }
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

BuildCheckResult InspectLoadedGame() {
    BuildCheckResult result;
    result.exe_path = GetProcessImagePath();
    ReadFileVersion(result.exe_path,
                    result.version_text,
                    result.version_major,
                    result.version_minor,
                    result.version_build,
                    result.version_private);

    const bool pe_ok = ReadPeMetadata(
        result.exe_path, result.file_size, result.time_date_stamp, result.size_of_image);
    const bool hash_ok = ComputeSha256(result.exe_path, result.sha256);

    const bool version_ok = result.version_major == build_info::kSupportedVersionMajor &&
                            result.version_minor == build_info::kSupportedVersionMinor &&
                            result.version_build == build_info::kSupportedVersionBuild &&
                            result.version_private == build_info::kSupportedVersionPrivate;
    const bool file_size_ok = result.file_size == build_info::kKnownFileSize;
    const bool image_ok = result.size_of_image == build_info::kKnownSizeOfImage;

    const bool legacy_match = hash_ok &&
                              result.time_date_stamp == build_info::kLegacyTimeDateStamp &&
                              result.sha256 == build_info::kLegacySha256;
    const bool latest_steam_match =
        hash_ok && result.time_date_stamp == build_info::kLatestSteamTimeDateStamp &&
        result.sha256 == build_info::kLatestSteamSha256;

    result.supported = pe_ok && version_ok && file_size_ok && image_ok &&
                       (legacy_match || latest_steam_match);
    result.hook_layout_supported = legacy_match || latest_steam_match;
    if (legacy_match) {
        result.build_id = "legacy_researched";
    } else if (latest_steam_match) {
        result.build_id = "latest_steam";
    } else {
        result.build_id = "unknown";
    }

    const char* hook_layout_text = "safe_mode";
    if (legacy_match) {
        hook_layout_text = "legacy_researched";
    } else if (latest_steam_match) {
        hook_layout_text = "latest_steam";
    }

    std::ostringstream summary;
    summary << "exe=" << Narrow(result.exe_path)
            << " version=" << Narrow(result.version_text)
            << " file_size=" << result.file_size
            << " timestamp=0x" << std::hex << std::uppercase << result.time_date_stamp
            << " size_of_image=0x" << result.size_of_image << std::dec
            << " sha256=" << HexString(result.sha256)
            << " supported=" << (result.supported ? "yes" : "no")
            << " hook_layout=" << hook_layout_text
            << " build_id=" << result.build_id;
    result.summary = summary.str();

    return result;
}

}  // namespace spatch
