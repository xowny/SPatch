#include "../src/ArchiveEntryPolicy.h"
#include "../src/AverageWindowPolicy.h"
#include "../src/BootstrapPolicy.h"
#include "../src/ChunkStreamPolicy.h"
#include "../src/BuildInfo.h"
#include "../src/CharacterEyeFix.h"
#include "../src/CharacterSweatPolicy.h"
#include "../src/CharacterWetnessPolicy.h"
#include "../src/Config.h"
#include "../src/CutContentProbe.h"
#include "../src/CutsceneTiming.h"
#include "../src/DisplaySettings.h"
#include "../src/EngineFixes.h"
#include "../src/FogRestorationPolicy.h"
#include "../src/FogSlicingPolicy.h"
#if !defined(SPATCH_FINAL_RELEASE)
#include "../src/HooksSummary.h"
#endif
#include "../src/InputPolicy.h"
#include "../src/Logger.h"
#include "../src/NisActorProbe.h"
#include "../src/PedestrianTiming.h"
#include "../src/QFileIoPolicy.h"
#include "../src/QcmpPolicy.h"
#include "../src/RuntimePatch.h"
#include "../src/SmaaRuntime.h"
#include "../src/SystemLibrary.h"
#include "../src/TextureFilteringPolicy.h"
#include "../src/VersionGuard.h"
#include "HookTargetGuardTests.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

static_assert(spatch::kConfigVersion == 44,
              "review the default-config migration contract before bumping it");

BOOL WINAPI
FailReadFile(HANDLE, LPVOID, DWORD, LPDWORD bytes_read, LPOVERLAPPED) {
    if (bytes_read != nullptr) {
        *bytes_read = 0;
    }
    SetLastError(ERROR_READ_FAULT);
    return FALSE;
}

BOOL WINAPI OversizedReadFile(HANDLE,
                              LPVOID,
                              DWORD buffer_size,
                              LPDWORD bytes_read,
                              LPOVERLAPPED) {
    if (bytes_read != nullptr) {
        *bytes_read = buffer_size + 1;
    }
    return TRUE;
}

bool ReturnLegacyHash(const std::filesystem::path&,
                      std::array<unsigned char, 32>& hash_out) {
    hash_out = spatch::build_info::kLegacySha256;
    return true;
}

std::uint32_t FloatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void WriteU32(std::span<std::byte> bytes,
              std::size_t offset,
              std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::uint32_t ReadU32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::array<std::byte, spatch::character_eye::kRequiredMaterialBytes>
MakeWeiEyeMaterial(
    std::uint32_t material_uid = spatch::character_eye::kWeiHdEyeMaterialUid,
    std::uint32_t bump_uid = spatch::character_eye::kWeiHeadHdBumpUid) {
    using namespace spatch::character_eye;
    std::array<std::byte, kRequiredMaterialBytes> material{};
    WriteU32(material, kMaterialNameUidOffset, material_uid);
    WriteU32(material, 0x30, 0xB4C26312);
    WriteU32(material, 0x70, 7);

    const auto set_param =
        [&material](std::size_t index, std::uint32_t state_uid,
                    std::uint32_t value_type_uid, std::uint32_t resource_uid,
                    std::uint32_t resource_type_uid) {
            const std::size_t base = 0x80 + index * 0x38;
            WriteU32(material, base, state_uid);
            WriteU32(material, base + 0x04, value_type_uid);
            WriteU32(material, base + 0x28, resource_uid);
            WriteU32(material, base + 0x30, resource_type_uid);
        };
    set_param(0, 0x5C19C934, 0x5C19C934, 0x84889C9C, 0x8B5561A1);
    set_param(3, 0xDCE06689, 0xC8377453, kDefinitiveFallbackDiffuseUid,
              0x8B43FABF);
    set_param(4, 0xADBE1A5A, 0xC8377453, bump_uid, 0x8B43FABF);
    set_param(5, 0x0490650C, 0xC8377453, 0x785D3471, 0x8B43FABF);
    return material;
}

bool ReturnHashFailure(const std::filesystem::path&,
                       std::array<unsigned char, 32>&) {
    return false;
}

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool g_fake_qfile_ready = true;
bool g_fake_qfile_seek_result = true;
bool g_fake_qfile_write_disk_full = false;
std::uint64_t g_fake_qfile_read_result = 0;
std::uint64_t g_fake_qfile_write_result = 0;
unsigned int g_fake_qfile_seek_calls = 0;
unsigned int g_fake_qfile_read_calls = 0;
unsigned int g_fake_qfile_write_calls = 0;

bool FakeQFileReady(void*) {
    return g_fake_qfile_ready;
}

bool FakeQFileSeek(void*, void*, std::uint32_t, std::int64_t) {
    ++g_fake_qfile_seek_calls;
    return g_fake_qfile_seek_result;
}

std::uint64_t FakeQFileRead(void*, void*, void*, std::uint64_t) {
    ++g_fake_qfile_read_calls;
    return g_fake_qfile_read_result;
}

std::uint64_t FakeQFileWrite(void*,
                             void*,
                             const void*,
                             std::uint64_t,
                             bool* disk_full) {
    ++g_fake_qfile_write_calls;
    if (disk_full != nullptr) {
        *disk_full = g_fake_qfile_write_disk_full;
    }
    return g_fake_qfile_write_result;
}

struct TestTempRoot {
    std::filesystem::path path;

    ~TestTempRoot() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

const std::filesystem::path& GetTestTempRoot() {
    static TestTempRoot root = [] {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path();
        std::array<wchar_t, MAX_PATH> unique_path{};
        if (GetTempFileNameW(
                base.c_str(), L"SPT", 0, unique_path.data()) != 0) {
            DeleteFileW(unique_path.data());
            if (CreateDirectoryW(unique_path.data(), nullptr) != FALSE) {
                const std::filesystem::path created(unique_path.data());
                return TestTempRoot{created};
            }
        }
        const std::filesystem::path fallback =
            base / (L"SPatchTests-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(fallback);
        return TestTempRoot{fallback};
    }();
    return root.path;
}

std::filesystem::path MakeTempIniPath(const wchar_t* name) {
    return GetTestTempRoot() / name;
}

bool InitializeTestLocalAppData() {
    const std::filesystem::path local_app_data =
        GetTestTempRoot() / L"LocalAppData";
    std::error_code error;
    std::filesystem::create_directories(local_app_data, error);
    return !error &&
           SetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.c_str()) !=
               FALSE;
}

std::filesystem::path InGameLegacyConfigBackupPath(
    const std::filesystem::path& config_path) {
    return std::filesystem::path(config_path.wstring() + L".previous.bak");
}

bool HasInGameConfigWorkResidue(const std::filesystem::path& config_path) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(config_path.parent_path(),
                                                       error),
         end;
         !error && iterator != end; iterator.increment(error)) {
        const std::wstring filename = iterator->path().filename().wstring();
        const std::wstring config_filename = config_path.filename().wstring();
        if (filename == config_filename + L".previous.bak" ||
            filename.starts_with(config_filename + L".spatch.")) {
            return true;
        }
    }
    return static_cast<bool>(error);
}

void RemoveIfExists(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

bool WriteMinimalPeFile(const std::filesystem::path& path,
                        unsigned long time_date_stamp,
                        unsigned long size_of_image,
                        unsigned short machine = IMAGE_FILE_MACHINE_AMD64,
                        std::size_t file_size = 512) {
    std::array<unsigned char, 512> buffer{};
    if (file_size < buffer.size()) {
        return false;
    }

    auto* dos_header = reinterpret_cast<IMAGE_DOS_HEADER*>(buffer.data());
    dos_header->e_magic = IMAGE_DOS_SIGNATURE;
    dos_header->e_lfanew = 0x80;

    auto* nt_headers = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        buffer.data() + dos_header->e_lfanew);
    nt_headers->Signature = IMAGE_NT_SIGNATURE;
    nt_headers->FileHeader.Machine = machine;
    nt_headers->FileHeader.SizeOfOptionalHeader =
        sizeof(IMAGE_OPTIONAL_HEADER64);
    nt_headers->FileHeader.TimeDateStamp = time_date_stamp;
    nt_headers->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt_headers->OptionalHeader.SizeOfImage = size_of_image;

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    stream.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(buffer.size()));
    if (file_size > buffer.size()) {
        stream.seekp(static_cast<std::streamoff>(file_size - 1), std::ios::beg);
        stream.put('\0');
    }
    return stream.good();
}

}  // namespace

// This executable deliberately keeps the complete integration matrix in one
// entry point. Its 100 KiB aggregate test-only frame remains well below the
// Windows x64 default stack reserve; production code is not affected.
#pragma warning(suppress : 6262)
int main(int argc, char* argv[]) {
    if (!InitializeTestLocalAppData()) {
        std::cerr << "Failed to isolate LOCALAPPDATA for config tests\n";
        return EXIT_FAILURE;
    }

    if (argc == 3 && std::string(argv[1]) == "--write-default-config") {
        const std::filesystem::path output(argv[2]);
        if (std::filesystem::exists(output)) {
            std::cerr << "Refusing to replace existing config: " << output
                      << '\n';
            return EXIT_FAILURE;
        }
        (void)spatch::LoadConfig(output);
        if (!std::filesystem::exists(output)) {
            std::cerr << "Failed to create default config: " << output << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Wrote default config: " << output << '\n';
        return EXIT_SUCCESS;
    }

    {
        HMODULE dbghelp = spatch::LoadSystemLibrary(L"dbghelp.dll");
        std::array<wchar_t, MAX_PATH> system_directory{};
        std::array<wchar_t, 32768> loaded_path{};
        const UINT system_length = GetSystemDirectoryW(
            system_directory.data(),
            static_cast<UINT>(system_directory.size()));
        const DWORD loaded_length = dbghelp == nullptr
                                        ? 0
                                        : GetModuleFileNameW(
                                              dbghelp,
                                              loaded_path.data(),
                                              static_cast<DWORD>(loaded_path.size()));
        std::error_code identity_error;
        const bool expected_identity =
            system_length != 0 && system_length < system_directory.size() &&
            loaded_length != 0 && loaded_length < loaded_path.size() &&
            std::filesystem::equivalent(
                std::filesystem::path(system_directory.data()) /
                    L"dbghelp.dll",
                std::filesystem::path(loaded_path.data()),
                identity_error) &&
            !identity_error;
        if (dbghelp != nullptr) {
            FreeLibrary(dbghelp);
        }
        if (!Expect(expected_identity &&
                        spatch::LoadSystemLibrary(L"..\\dbghelp.dll") == nullptr &&
                        spatch::LoadSystemLibrary(L"C:\\Windows\\dbghelp.dll") ==
                            nullptr,
                    "system-library loading should reject redirected names and "
                    "resolve the requested Windows component from System32")) {
            return EXIT_FAILURE;
        }
    }


    if (!spatch::tests::RunHookTargetGuardTests()) {
        return EXIT_FAILURE;
    }

    {
        using namespace spatch::qfile_io;

        alignas(16) std::array<std::byte, 0xA0> file{};
        std::array<void*, kWriteVtableSlot + 1> vtable{};
        struct FakeDevice {
            void** vtable = nullptr;
        } device{vtable.data()};
        void* device_pointer = &device;
        std::memcpy(file.data() + kDeviceOffset, &device_pointer,
                    sizeof(device_pointer));
        vtable[kSeekVtableSlot] = reinterpret_cast<void*>(&FakeQFileSeek);
        vtable[kReadVtableSlot] = reinterpret_cast<void*>(&FakeQFileRead);
        vtable[kWriteVtableSlot] = reinterpret_cast<void*>(&FakeQFileWrite);

        auto* const critical_section = reinterpret_cast<LPCRITICAL_SECTION>(
            file.data() + kOperationCriticalSectionOffset);
        InitializeCriticalSection(critical_section);

        std::array<std::byte, 16> buffer{};
        g_fake_qfile_ready = true;
        g_fake_qfile_seek_result = false;
        g_fake_qfile_seek_calls = 0;
        g_fake_qfile_read_calls = 0;
        g_fake_qfile_write_calls = 0;
        bool disk_full = true;
        const std::uint64_t failed_read =
            ReadAt(&FakeQFileReady, file.data(), buffer.data(), buffer.size(),
                   64, FILE_BEGIN);
        const std::uint64_t failed_write =
            WriteAt(&FakeQFileReady, file.data(), buffer.data(), buffer.size(),
                    64, FILE_BEGIN, &disk_full);
        if (!Expect(
                failed_read == kOperationFailure &&
                    failed_write == kOperationFailure &&
                    g_fake_qfile_seek_calls == 2 &&
                    g_fake_qfile_read_calls == 0 &&
                    g_fake_qfile_write_calls == 0 && !disk_full,
                "qFile offset operations must stop when their seek fails")) {
            DeleteCriticalSection(critical_section);
            return EXIT_FAILURE;
        }

        g_fake_qfile_seek_result = true;
        g_fake_qfile_read_result = buffer.size();
        g_fake_qfile_write_result = buffer.size();
        g_fake_qfile_write_disk_full = false;
        const std::uint64_t completed_read =
            ReadAt(&FakeQFileReady, file.data(), buffer.data(), buffer.size(),
                   8, FILE_BEGIN);
        const std::uint64_t completed_write =
            WriteAt(&FakeQFileReady, file.data(), buffer.data(), buffer.size(),
                    8, FILE_BEGIN, &disk_full);
        if (!Expect(completed_read == buffer.size() &&
                        completed_write == buffer.size() &&
                        g_fake_qfile_read_calls == 1 &&
                        g_fake_qfile_write_calls == 1 && !disk_full,
                    "qFile offset operations should preserve successful device "
                    "results")) {
            DeleteCriticalSection(critical_section);
            return EXIT_FAILURE;
        }
        DeleteCriticalSection(critical_section);

        const std::filesystem::path path =
            MakeTempIniPath(L"SPatch-qfile-io-test.tmp");
        HANDLE handle = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (!Expect(handle != INVALID_HANDLE_VALUE,
                    "qFile device test should create a temporary file")) {
            return EXIT_FAILURE;
        }

        alignas(16) std::array<std::byte, 0x60> native_file{};
        std::memcpy(native_file.data() + kNativeHandleOffset, &handle,
                    sizeof(handle));
        constexpr std::array<char, 4> payload{'a', 'b', 'c', 'd'};
        DWORD bytes_written = 0;
        const bool seeded = WriteFile(handle, payload.data(),
                                      static_cast<DWORD>(payload.size()),
                                      &bytes_written, nullptr) != FALSE &&
                            bytes_written == static_cast<DWORD>(payload.size());
        char value = '\0';
        const bool device_contract =
            seeded &&
            SizeFileDevice(nullptr, native_file.data()) == payload.size() &&
            SeekFileDevice(nullptr, native_file.data(), FILE_BEGIN, 1) &&
            TellFileDevice(nullptr, native_file.data()) == 1 &&
            ReadFileDevice(nullptr, native_file.data(), &value, 1) == 1 &&
            value == 'b';
        if (!Expect(device_contract,
                    "qFile device helpers should preserve "
                    "successful 64-bit file operations")) {
            CloseHandle(handle);
            return EXIT_FAILURE;
        }

        std::array<char, 2> short_buffer{};
        const bool short_read_rejected =
            SeekFileDevice(nullptr, native_file.data(), FILE_BEGIN, 3) &&
            ReadFileDevice(nullptr, native_file.data(), short_buffer.data(),
                           short_buffer.size()) == kOperationFailure;
        if (!Expect(short_read_rejected,
                    "qFile device reads must reject successful Win32 reads "
                    "that return fewer bytes than requested")) {
            CloseHandle(handle);
            return EXIT_FAILURE;
        }

        HANDLE invalid_handle = INVALID_HANDLE_VALUE;
        std::memcpy(native_file.data() + kNativeHandleOffset, &invalid_handle,
                    sizeof(invalid_handle));
        if (!Expect(
                !SeekFileDevice(nullptr, native_file.data(), FILE_BEGIN, 0) &&
                    TellFileDevice(nullptr, native_file.data()) ==
                        kOperationFailure &&
                    SizeFileDevice(nullptr, native_file.data()) ==
                        kOperationFailure &&
                    ReadFileDevice(nullptr, native_file.data(), &value, 1) ==
                        kOperationFailure,
                "qFile device helpers must preserve Win32 failure sentinels")) {
            CloseHandle(handle);
            return EXIT_FAILURE;
        }
        // Deliberately invalid addresses exercise the guarded engine-pointer
        // path without dereferencing them in the test process.
        // cppcheck-suppress intToPointerCast
        auto* const inaccessible_file = reinterpret_cast<void*>(1);
        // cppcheck-suppress intToPointerCast
        auto* const inaccessible_disk_full = reinterpret_cast<bool*>(1);
        if (!Expect(
                NativeHandle(inaccessible_file) == nullptr &&
                    FileDevice(inaccessible_file) == nullptr &&
                    ReadAt(&FakeQFileReady,
                           inaccessible_file,
                           &value,
                           1,
                           0,
                           FILE_BEGIN) == kOperationFailure &&
                    WriteAt(&FakeQFileReady,
                            inaccessible_file,
                            &value,
                            1,
                            0,
                            FILE_BEGIN,
                            inaccessible_disk_full) == kOperationFailure,
                "qFile helpers must fail closed for inaccessible engine pointers")) {
            CloseHandle(handle);
            return EXIT_FAILURE;
        }
        CloseHandle(handle);
    }

    {
        using namespace spatch::qcmp;

        const auto make_stream = [](std::initializer_list<std::uint8_t> tokens,
                                    std::uint64_t output_size,
                                    bool swapped = false) {
            std::vector<std::byte> stream(kHeaderSize + tokens.size());
            const std::uint32_t magic =
                swapped ? kSwappedMagic : kCanonicalMagic;
            std::uint16_t type = kLzType;
            std::uint16_t version = kFormatVersion;
            std::uint32_t data_offset = static_cast<std::uint32_t>(kHeaderSize);
            std::uint64_t stream_end = stream.size();
            if (swapped) {
                type = ByteSwap16(type);
                version = ByteSwap16(version);
                data_offset = ByteSwap32(data_offset);
                stream_end = ByteSwap64(stream_end);
                output_size = ByteSwap64(output_size);
            }
            std::memcpy(stream.data(), &magic, sizeof(magic));
            std::memcpy(stream.data() + 0x04, &type, sizeof(type));
            std::memcpy(stream.data() + 0x06, &version, sizeof(version));
            std::memcpy(stream.data() + 0x08,
                        &data_offset,
                        sizeof(data_offset));
            std::memcpy(stream.data() + 0x10,
                        &stream_end,
                        sizeof(stream_end));
            std::memcpy(stream.data() + 0x18,
                        &output_size,
                        sizeof(output_size));
            std::size_t index = kHeaderSize;
            for (const std::uint8_t token : tokens) {
                stream[index++] = static_cast<std::byte>(token);
            }
            return stream;
        };

        const auto canonical = make_stream(
            {0x02, 'a', 'b', 'c', 0x40, 0x03, 0x20}, 9);
        const auto swapped = make_stream(
            {0x02, 'a', 'b', 'c', 0x40, 0x03, 0x20}, 9, true);
        const auto extended =
            make_stream({0x00, 'x', 0xE0, 0x01, 0x04}, 6);
        std::array<char, 9> decoded{};
        std::array<char, 9> swapped_decoded{};
        std::array<char, 6> extended_decoded{};
        const auto swapped_before = swapped;
        if (!Expect(Validate(canonical.data(), canonical.size(), 9).valid() &&
                        Validate(canonical.data(), canonical.size(), 9)
                                .output_size == 9 &&
                        Validate(swapped.data(), swapped.size(), 9).valid() &&
                        Validate(extended.data(), extended.size(), 6).valid() &&
                        Validate(extended.data(), extended.size(), 6)
                                .output_size == 6 &&
                        Decode(canonical.data(), canonical.size(),
                               decoded.data(), decoded.size())
                                .output_size == decoded.size() &&
                        std::memcmp(decoded.data(), "abcabcabc",
                                    decoded.size()) == 0 &&
                        Decode(swapped.data(), swapped.size(),
                               swapped_decoded.data(), swapped_decoded.size())
                                .output_size == swapped_decoded.size() &&
                        std::memcmp(swapped_decoded.data(), "abcabcabc",
                                    swapped_decoded.size()) == 0 &&
                        swapped == swapped_before &&
                        Decode(extended.data(), extended.size(),
                               extended_decoded.data(), extended_decoded.size())
                                .output_size == extended_decoded.size() &&
                        std::memcmp(extended_decoded.data(), "xxxxxx",
                                    extended_decoded.size()) == 0,
                    "QCMP validation should preserve canonical, byte-swapped, "
                    "cached-pattern, extended-length, and single-pass decode "
                    "semantics")) {
            return EXIT_FAILURE;
        }

        const auto truncated_literal = make_stream({0x03, 'a'}, 4);
        const auto missing_distance = make_stream({0x40}, 3);
        const auto early_backreference = make_stream({0x40, 0x01}, 3);
        const auto uninitialized_pattern = make_stream({0x20}, 3);
        auto invalid_magic = canonical;
        invalid_magic[0] = std::byte{0};
        auto invalid_type = canonical;
        const std::uint16_t type_zero = 0;
        std::memcpy(invalid_type.data() + 0x04, &type_zero,
                    sizeof(type_zero));
        auto invalid_version = canonical;
        const std::uint16_t version_two = 2;
        std::memcpy(invalid_version.data() + 0x06, &version_two,
                    sizeof(version_two));
        auto early_data = canonical;
        const std::uint32_t early_offset = 0x20;
        std::memcpy(early_data.data() + 0x08,
                    &early_offset,
                    sizeof(early_offset));
        auto late_data = canonical;
        const std::uint32_t late_offset = 0x44;
        std::memcpy(late_data.data() + 0x08, &late_offset,
                    sizeof(late_offset));
        auto stream_before_data = canonical;
        const std::uint64_t early_end = 0x20;
        std::memcpy(stream_before_data.data() + 0x10, &early_end,
                    sizeof(early_end));
        auto oversized_stream = canonical;
        const std::uint64_t oversized_end = canonical.size() + 1;
        std::memcpy(oversized_stream.data() + 0x10,
                    &oversized_end,
                    sizeof(oversized_end));
        auto trailing_source = canonical;
        trailing_source.push_back(std::byte{0});
        auto declared_too_large = canonical;
        const std::uint64_t declared_ten = 10;
        std::memcpy(declared_too_large.data() + 0x18, &declared_ten,
                    sizeof(declared_ten));
        auto output_mismatch = canonical;
        const std::uint64_t declared_eight = 8;
        std::memcpy(output_mismatch.data() + 0x18, &declared_eight,
                    sizeof(declared_eight));
        if (!Expect(
                Validate(nullptr, canonical.size(), 9).error ==
                        Error::NullBuffer &&
                    Decode(canonical.data(), canonical.size(), nullptr, 9)
                            .error == Error::NullDestination &&
                    Validate(canonical.data(), kHeaderSize - 1, 9).error ==
                        Error::SourceSmallerThanHeader &&
                    Validate(canonical.data(), kFailure, 9).error ==
                        Error::InvalidSize &&
                    Validate(invalid_magic.data(), invalid_magic.size(), 9)
                            .error == Error::InvalidMagic &&
                    Validate(invalid_type.data(), invalid_type.size(), 9)
                            .error == Error::InvalidType &&
                    Validate(invalid_version.data(), invalid_version.size(), 9)
                            .error == Error::InvalidVersion &&
                    Validate(early_data.data(), early_data.size(), 9).error ==
                        Error::DataOffsetBeforeHeader &&
                    Validate(late_data.data(), late_data.size(), 9).error ==
                        Error::UnexpectedDataOffset &&
                    Validate(stream_before_data.data(),
                             stream_before_data.size(), 9)
                            .error == Error::StreamEndBeforeData &&
                    Validate(oversized_stream.data(), oversized_stream.size(),
                             9)
                            .error == Error::StreamEndAfterSource &&
                    Validate(trailing_source.data(), trailing_source.size(), 9)
                            .error == Error::CompressedSizeMismatch &&
                    Validate(declared_too_large.data(),
                             declared_too_large.size(), 9)
                            .error ==
                        Error::DeclaredOutputExceedsDestination &&
                    Validate(truncated_literal.data(),
                             truncated_literal.size(),
                             9)
                            .error == Error::LiteralCrossesStream &&
                    Validate(missing_distance.data(), missing_distance.size(),
                             9)
                            .error == Error::TokenCrossesStream &&
                    Validate(early_backreference.data(),
                             early_backreference.size(),
                             9)
                            .error == Error::BackreferenceBeforeOutput &&
                    Validate(uninitialized_pattern.data(),
                             uninitialized_pattern.size(),
                             9)
                            .error == Error::UninitializedPattern &&
                    Validate(output_mismatch.data(), output_mismatch.size(), 8)
                            .error == Error::OutputExceedsDestination &&
                    Validate(output_mismatch.data(), output_mismatch.size(), 9)
                            .error == Error::OutputSizeMismatch &&
                    std::strcmp(ErrorName(Error::BufferAccessException),
                                "buffer_access_exception") == 0,
                "QCMP validation must reject invalid headers, incomplete "
                "streams, and every stock input/output out-of-bounds path")) {
            return EXIT_FAILURE;
        }
    }

    {
        using spatch::chunk_stream::Header;

        std::array<std::byte, 36> chunks{};
        const Header first{0x10203040, 3, 100, 999};
        const Header second{0x50607080, 0, 0, 0};
        std::memcpy(chunks.data(), &first, sizeof(first));
        std::memcpy(chunks.data() + 20, &second, sizeof(second));
        const auto valid =
            spatch::chunk_stream::Validate(chunks.data(), chunks.size());
        if (!Expect(valid.valid() && valid.chunk_count == 2 &&
                        valid.offset == chunks.size(),
                    "chunk stream bounds should preserve aligned multi-chunk "
                    "files without interpreting handler-specific data fields") ||
            !Expect(spatch::chunk_stream::Validate(nullptr, 0).valid(),
                    "empty chunk streams should remain a stock no-op") ||
            !Expect(spatch::chunk_stream::Validate(nullptr, 1).error ==
                        spatch::chunk_stream::Error::NullBuffer,
                    "non-empty chunk streams require a buffer") ||
            !Expect(std::strcmp(spatch::chunk_stream::ErrorName(
                                    spatch::chunk_stream::Error::
                                        BufferAccessException),
                                "buffer_access_exception") == 0,
                    "guarded chunk-stream faults require a stable log reason")) {
            return EXIT_FAILURE;
        }

        std::array<std::byte, 15> truncated_header{};
        std::array<std::byte, sizeof(Header)> negative_chunk{};
        std::array<std::byte, sizeof(Header)> oversized_chunk{};
        Header invalid{1, -1, 0, 0};
        std::memcpy(negative_chunk.data(), &invalid, sizeof(invalid));
        invalid.chunk_size = 4;
        std::memcpy(oversized_chunk.data(), &invalid, sizeof(invalid));
        if (!Expect(spatch::chunk_stream::Validate(truncated_header.data(),
                                                   truncated_header.size())
                            .error ==
                        spatch::chunk_stream::Error::TruncatedHeader &&
                        spatch::chunk_stream::Validate(negative_chunk.data(),
                                                       negative_chunk.size())
                                .error ==
                            spatch::chunk_stream::Error::NegativeChunkSize &&
                        spatch::chunk_stream::Validate(oversized_chunk.data(),
                                                       oversized_chunk.size())
                                .error ==
                            spatch::chunk_stream::Error::ChunkExceedsBuffer &&
                        spatch::chunk_stream::Validate(chunks.data(), 21).error ==
                            spatch::chunk_stream::Error::TruncatedHeader,
                    "chunk stream bounds must reject every stock header and "
                    "advance overread path")) {
            return EXIT_FAILURE;
        }

        constexpr std::size_t payload_size =
            spatch::big_file_index::kSerializedMetadataSize +
            2 * spatch::big_file_index::kEntrySize;
        std::array<std::byte, payload_size> index{};
        const std::uint32_t type = spatch::big_file_index::kTypeId;
        const std::uint32_t count = 2;
        const std::int64_t entries_relative =
            static_cast<std::int64_t>(
                spatch::big_file_index::kSerializedMetadataSize) -
            static_cast<std::int64_t>(
                spatch::big_file_index::kEntriesRelativeOffset);
        const std::uint32_t first_uid = 10;
        const std::uint32_t second_uid = 20;
        std::memcpy(index.data() + spatch::big_file_index::kTypeIdOffset,
                    &type,
                    sizeof(type));
        std::memcpy(index.data() + spatch::big_file_index::kEntryCountOffset,
                    &count,
                    sizeof(count));
        std::memcpy(index.data() +
                        spatch::big_file_index::kEntriesRelativeOffset,
                    &entries_relative,
                    sizeof(entries_relative));
        std::memcpy(index.data() +
                        spatch::big_file_index::kSerializedMetadataSize,
                    &first_uid,
                    sizeof(first_uid));
        std::memcpy(index.data() +
                        spatch::big_file_index::kSerializedMetadataSize +
                        spatch::big_file_index::kEntrySize,
                    &second_uid,
                    sizeof(second_uid));

        const auto valid_index = spatch::big_file_index::Validate(
            index.data(), index.size(), static_cast<std::int32_t>(index.size()), 0);
        if (!Expect(valid_index.valid() && valid_index.entry_count == 2 &&
                        valid_index.entries_offset ==
                            spatch::big_file_index::kSerializedMetadataSize,
                    "BIG file indexes should accept a bounded sorted entry "
                    "array")) {
            return EXIT_FAILURE;
        }

        auto invalid_index = index;
        const std::uint32_t too_many = 3;
        std::memcpy(invalid_index.data() +
                        spatch::big_file_index::kEntryCountOffset,
                    &too_many,
                    sizeof(too_many));
        if (!Expect(
                spatch::big_file_index::Validate(
                    invalid_index.data(),
                    invalid_index.size(),
                    static_cast<std::int32_t>(invalid_index.size()),
                    0)
                        .error ==
                    spatch::big_file_index::Error::EntryArrayExceedsPayload,
                "BIG file index counts must not extend the entry array past "
                "the loaded chunk")) {
            return EXIT_FAILURE;
        }

        invalid_index = index;
        const std::uint32_t descending_uid = 5;
        std::memcpy(invalid_index.data() +
                        spatch::big_file_index::kSerializedMetadataSize +
                        spatch::big_file_index::kEntrySize,
                    &descending_uid,
                    sizeof(descending_uid));
        const auto unsorted = spatch::big_file_index::Validate(
            invalid_index.data(),
            invalid_index.size(),
            static_cast<std::int32_t>(invalid_index.size()),
            0);
        if (!Expect(unsorted.error ==
                            spatch::big_file_index::Error::EntriesNotSorted &&
                        unsorted.offending_index == 1,
                    "BIG file indexes must preserve the binary-search sort "
                    "contract")) {
            return EXIT_FAILURE;
        }

        invalid_index = index;
        const std::int64_t pointer_inside_metadata = 0;
        std::memcpy(invalid_index.data() +
                        spatch::big_file_index::kEntriesRelativeOffset,
                    &pointer_inside_metadata,
                    sizeof(pointer_inside_metadata));
        if (!Expect(
                spatch::big_file_index::Validate(
                    invalid_index.data(),
                    invalid_index.size(),
                    static_cast<std::int32_t>(invalid_index.size()),
                    0)
                        .error ==
                    spatch::big_file_index::Error::EntriesPointerInsideMetadata,
                "BIG file index relative pointers must not alias metadata")) {
            return EXIT_FAILURE;
        }

        std::vector<std::byte> index_chunk(
            spatch::chunk_stream::kHeaderSize + index.size());
        const Header index_header{
            spatch::big_file_index::kChunkId,
            static_cast<std::int32_t>(index.size()),
            static_cast<std::int32_t>(index.size()),
            0};
        std::memcpy(index_chunk.data(), &index_header, sizeof(index_header));
        std::memcpy(index_chunk.data() + spatch::chunk_stream::kHeaderSize,
                    index.data(),
                    index.size());
        if (!Expect(spatch::chunk_stream::Validate(index_chunk.data(),
                                                   index_chunk.size())
                            .valid(),
                    "chunk stream validation should accept a valid BIG file "
                    "inventory") ||
            !Expect(spatch::big_file_index::Validate(
                        index.data(),
                        index.size(),
                        static_cast<std::int32_t>(index.size() - 1),
                        0)
                            .error ==
                        spatch::big_file_index::Error::
                            ChunkAndDataSizeMismatch,
                    "BIG file inventory chunks require matching serialized "
                    "and data sizes") ||
            !Expect(spatch::big_file_index::Validate(
                        index.data(),
                        index.size(),
                        static_cast<std::int32_t>(index.size()),
                        1)
                            .error ==
                        spatch::big_file_index::Error::UnexpectedDataOffset,
                    "BIG file inventory chunks require the stock zero data "
                    "offset")) {
            return EXIT_FAILURE;
        }
    }

    {
        using namespace spatch::archive_io;

        ReadRange range{};
        EntryDescriptor raw{};
        raw.uid = 0x10203040;
        raw.offset_divided_by_four = 25;
        raw.load_offset = 0xABC;
        raw.uncompressed_size = 12;
        if (!Expect(IsWithinArchive(raw, 112, &range) && range.offset == 100 &&
                        range.size == 12 && range.end == 112,
                    "raw archive entries should permit reads ending exactly at "
                    "EOF") ||
            !Expect(
                !IsWithinArchive(raw, 111),
                "raw archive entries should reject reads extending past EOF")) {
            return EXIT_FAILURE;
        }

        EntryDescriptor compressed{};
        compressed.uid = 0x50607080;
        compressed.offset_divided_by_four = 10;
        compressed.load_offset = 0x1234;
        compressed.compressed_size = 100;
        compressed.allocation_extra = 0xFFFFFFFF;
        compressed.uncompressed_size = 1000;
        if (!Expect(IsWithinArchive(compressed, 704, &range) &&
                        range.offset == 40 && range.size == 664 &&
                        range.end == 704,
                    "compressed archive bounds should include only the on-disk "
                    "prefix") ||
            !Expect(!IsWithinArchive(compressed, 703),
                    "compressed archive entries should reject truncated "
                    "payloads")) {
            return EXIT_FAILURE;
        }

        EntryDescriptor stored_with_metadata = compressed;
        stored_with_metadata.compressed_size =
            stored_with_metadata.uncompressed_size;
        if (!Expect(IsWithinArchive(stored_with_metadata, 1040, &range) &&
                        range.size == 1000 && range.end == 1040,
                    "stored archive entries should ignore the compressed-read "
                    "prefix")) {
            return EXIT_FAILURE;
        }

        EntryDescriptor maximum_offset{};
        maximum_offset.offset_divided_by_four =
            (std::numeric_limits<std::uint32_t>::max)();
        maximum_offset.uncompressed_size = 4;
        constexpr std::uint64_t maximum_end = 0x400000000ULL;
        if (!Expect(IsWithinArchive(maximum_offset, maximum_end, &range) &&
                        range.offset == 0x3FFFFFFFCULL &&
                        range.end == maximum_end,
                    "archive offsets should remain full-width above 4 GiB")) {
            return EXIT_FAILURE;
        }
    }
    using namespace spatch;

    {
        using namespace character_eye;
        struct EyeCase {
            std::uint32_t material_uid;
            std::uint32_t bump_uid;
            std::uint32_t diffuse_uid;
        };
        constexpr std::array eye_cases{
            EyeCase{kWeiHdEyeMaterialUid, kWeiHeadHdBumpUid,
                    kOriginalWeiHeadHdDiffuseUid},
            EyeCase{kWeiGangEyeMaterialUid, kWeiGangBumpUid,
                    kOriginalWeiHeadSdDiffuseUid},
            EyeCase{kWeiGangHdEyeMaterialUid, kWeiGangHdBumpUid,
                    kOriginalWeiHeadHdDiffuseUid},
        };
        std::uint32_t combined_log_bits = 0;
        for (const EyeCase& eye_case : eye_cases) {
            auto material =
                MakeWeiEyeMaterial(eye_case.material_uid, eye_case.bump_uid);
            if (!Expect(ApplyOriginalWeiEyeDiffuse(material) ==
                                RestoreResult::Applied &&
                            ReadU32(material, kDiffuseResourceUidOffset) ==
                                eye_case.diffuse_uid,
                        "Wei eye repair should select the variant's matching "
                        "diffuse") ||
                !Expect(ApplyOriginalWeiEyeDiffuse(material) ==
                            RestoreResult::AlreadyApplied,
                        "Wei eye repair should be idempotent") ||
                !Expect(
                    IsTargetMaterialUid(eye_case.material_uid) &&
                        OriginalDiffuseForMaterial(eye_case.material_uid) ==
                            eye_case.diffuse_uid &&
                        LogBitForMaterial(eye_case.material_uid) != 0 &&
                        (combined_log_bits &
                         LogBitForMaterial(eye_case.material_uid)) == 0,
                    "Wei eye variant metadata should be complete and unique")) {
                return EXIT_FAILURE;
            }
            combined_log_bits |= LogBitForMaterial(eye_case.material_uid);
        }

        auto non_target = MakeWeiEyeMaterial();
        WriteU32(non_target, kMaterialNameUidOffset, 0x12345678);
        if (!Expect(ApplyOriginalWeiEyeDiffuse(non_target) ==
                            RestoreResult::NotTarget &&
                        ReadU32(non_target, kDiffuseResourceUidOffset) ==
                            kDefinitiveFallbackDiffuseUid,
                    "eye repair should ignore every other material") ||
            !Expect(!IsTargetMaterialUid(0x12345678) &&
                        OriginalDiffuseForMaterial(0x12345678) == 0 &&
                        LogBitForMaterial(0x12345678) == 0,
                    "unknown eye variants should not expose replacement "
                    "metadata")) {
            return EXIT_FAILURE;
        }

        auto changed_layout = MakeWeiEyeMaterial();
        WriteU32(changed_layout, 0x80 + 4 * 0x38 + 0x28, 0xDEADBEEF);
        if (!Expect(ApplyOriginalWeiEyeDiffuse(changed_layout) ==
                            RestoreResult::InvalidLayout &&
                        ReadU32(changed_layout, kDiffuseResourceUidOffset) ==
                            kDefinitiveFallbackDiffuseUid,
                    "eye repair should fail closed when a companion texture "
                    "changes")) {
            return EXIT_FAILURE;
        }

        auto wrong_variant_diffuse =
            MakeWeiEyeMaterial(kWeiGangEyeMaterialUid, kWeiGangBumpUid);
        WriteU32(wrong_variant_diffuse, kDiffuseResourceUidOffset,
                 kOriginalWeiHeadHdDiffuseUid);
        if (!Expect(ApplyOriginalWeiEyeDiffuse(wrong_variant_diffuse) ==
                        RestoreResult::InvalidLayout,
                    "eye repair should reject another variant's diffuse")) {
            return EXIT_FAILURE;
        }

        auto truncated = MakeWeiEyeMaterial();
        if (!Expect(ApplyOriginalWeiEyeDiffuse(
                        std::span<std::byte>(truncated).first(
                            kRequiredMaterialBytes - 1)) ==
                        RestoreResult::InvalidLayout,
                    "eye repair should reject truncated material layouts")) {
            return EXIT_FAILURE;
        }
    }

    if (!Expect(
            cut_content::SymbolHash("M_CO") == 0x106E37DEu,
            "cut-content mission root should use the internal M_CO symbol") ||
        !Expect(cut_content::SymbolHash("E_TH") == 0xD05D1AB8u,
                "Triad Highway should use the internal E_TH symbol") ||
        !Expect(
            cut_content::SymbolHash("E_DTC") == 0xB7F394DFu,
            "Death By a Thousand Cuts should use the internal E_DTC symbol") ||
        !Expect(
            cut_content::SymbolHash("E_BrokenNoseCall") == 0x3679F77Cu,
            "mixed-case symbols should use case-sensitive qSymbol hashing")) {
        return EXIT_FAILURE;
    }

    {
        using namespace character_wetness;
        constexpr Timing timing{30.0f, 270.0f};

        State dry{};
        const StepResult idle = Advance(dry, 1.0f / 60.0f, false, 0.0f, timing);
        if (!Expect(!idle.active && idle.amount == 0.0f,
                    "wetness policy should not own a dry character")) {
            return EXIT_FAILURE;
        }

        MarkWaterCollision(dry, timing);
        if (!Expect(dry.amount == 1.0f &&
                        dry.hold_seconds == timing.full_wet_seconds,
                    "water collision should apply full wetness immediately")) {
            return EXIT_FAILURE;
        }

        for (int frame = 0; frame < 30 * 60; ++frame) {
            Advance(dry, 1.0f / 60.0f, false, 0.0f, timing);
        }
        if (!Expect(dry.amount > 0.999f,
                    "post-water hold should preserve full wetness")) {
            return EXIT_FAILURE;
        }
        for (int frame = 0; frame < 135 * 60; ++frame) {
            Advance(dry, 1.0f / 60.0f, false, 0.0f, timing);
        }
        if (!Expect(std::fabs(dry.amount - 0.5f) < 0.002f,
                    "wetness should fade linearly after the hold")) {
            return EXIT_FAILURE;
        }

        const auto simulate_drying = [timing](float frame_delta, int frames) {
            State state{};
            MarkWaterCollision(state, timing);
            for (int frame = 0; frame < frames; ++frame) {
                Advance(state, frame_delta, false, 0.0f, timing);
            }
            return state.amount;
        };
        const float at_30_fps = simulate_drying(1.0f / 30.0f, 165 * 30);
        const float at_240_fps = simulate_drying(1.0f / 240.0f, 165 * 240);
        if (!Expect(std::fabs(at_30_fps - at_240_fps) < 0.002f,
                    "wetness drying should be frame-rate independent")) {
            return EXIT_FAILURE;
        }

        State rain{};
        Advance(rain, 1.0f / 60.0f, true, 0.35f, timing);
        Advance(rain, 1.0f / 60.0f, true, 0.80f, timing);
        if (!Expect(std::fabs(rain.amount - 0.80f) < 0.0001f,
                    "rain should follow rising surface wetness")) {
            return EXIT_FAILURE;
        }
        const float before_invalid_delta = rain.amount;
        Advance(rain, std::numeric_limits<float>::quiet_NaN(), false, 0.0f,
                timing);
        if (!Expect(rain.amount == before_invalid_delta,
                    "invalid frame deltas should not advance drying")) {
            return EXIT_FAILURE;
        }

        State zero_duration{1.0f, 0.0f};
        const StepResult immediate =
            Advance(zero_duration, 1.0f / 60.0f, false, 0.0f, Timing{});
        if (!Expect(
                !immediate.active && immediate.amount == 0.0f,
                "zero fade time should remove wetness immediately after the "
                "full-wet period")) {
            return EXIT_FAILURE;
        }

        if (!Expect(!ShouldYieldToStrongerOwner(0.0f, 1.0f, 1.0f) &&
                        !ShouldYieldToStrongerOwner(0.4f, 0.8f, 0.6f) &&
                        ShouldYieldToStrongerOwner(0.9f, 0.6f, 0.7f),
                    "lower stock writes should not permanently disable wetness "
                    "ownership")) {
            return EXIT_FAILURE;
        }
    }

    {
        using namespace character_sweat;
        constexpr Timing timing{20.0f, 20.0f, 0.0f};

        State state{};
        if (!Expect(!Advance(state, 1.0f / 60.0f, false, timing).active,
                    "sweat policy should not own an idle dry character")) {
            return EXIT_FAILURE;
        }
        for (int frame = 0; frame < 20 * 60; ++frame) {
            Advance(state, 1.0f / 60.0f, true, timing);
        }
        if (!Expect(std::fabs(state.amount - 1.0f) < 0.002f,
                    "sustained exertion should build full sweat over the "
                    "configured time")) {
            return EXIT_FAILURE;
        }
        for (int frame = 0; frame < 10 * 60; ++frame) {
            Advance(state, 1.0f / 60.0f, false, timing);
        }
        if (!Expect(std::fabs(state.amount - 0.5f) < 0.002f,
                    "sweat should fade linearly after exertion stops")) {
            return EXIT_FAILURE;
        }

        const auto simulate_build = [timing](float frame_delta, int frames) {
            State simulated{};
            for (int frame = 0; frame < frames; ++frame) {
                Advance(simulated, frame_delta, true, timing);
            }
            return simulated.amount;
        };
        const float at_30_fps = simulate_build(1.0f / 30.0f, 10 * 30);
        const float at_240_fps = simulate_build(1.0f / 240.0f, 10 * 240);
        if (!Expect(std::fabs(at_30_fps - at_240_fps) < 0.002f &&
                        std::fabs(at_30_fps - 0.5f) < 0.002f,
                    "sweat accumulation should be frame-rate independent")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !IsRunning(2.49f) && IsRunning(2.5f) &&
                    !IsRunning(std::numeric_limits<float>::quiet_NaN()),
                "run detection should reject idle and invalid velocities")) {
            return EXIT_FAILURE;
        }

        State onset_state{};
        constexpr Timing onset_timing{20.0f, 20.0f, 5.0f};
        for (int frame = 0; frame < 4 * 60; ++frame) {
            Advance(onset_state, 1.0f / 60.0f, true, onset_timing);
        }
        if (!Expect(onset_state.amount == 0.0f,
                    "sweat onset should remain invisible during the configured "
                    "delay")) {
            return EXIT_FAILURE;
        }
        for (int frame = 0; frame < 2 * 60; ++frame) {
            Advance(onset_state, 1.0f / 60.0f, true, onset_timing);
        }
        if (!Expect(onset_state.amount > 0.0f && onset_state.amount < 0.2f,
                    "sweat should begin accumulating only after onset")) {
            return EXIT_FAILURE;
        }
    }

    {
        const auto path = MakeTempIniPath(L"spatch-logger-lifecycle-test.log");
        const auto second_path =
            MakeTempIniPath(L"spatch-logger-path-switch-test.log");
        RemoveIfExists(path);
        RemoveIfExists(second_path);
        if (!Expect(log::Initialize(path, false),
                    "disabled logging should initialize as a no-op")) {
            return EXIT_FAILURE;
        }
        log::Info("disabled logger must not create a file");
        log::Shutdown();
        if (!Expect(!std::filesystem::exists(path),
                    "disabled logging should have no file or formatting side effects")) {
            return EXIT_FAILURE;
        }
        if (!Expect(log::Initialize(path, true),
                    "logger should start its writer thread")) {
            return EXIT_FAILURE;
        }
        log::Info("logger lifecycle test");
        log::Shutdown();

        std::ifstream stream(path, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
        if (!Expect(contents.find("logger lifecycle test") != std::string::npos,
                    "logger shutdown should flush queued lines")) {
            return EXIT_FAILURE;
        }
        if (!Expect(log::Initialize(path, true),
                    "logger should support a clean restart")) {
            return EXIT_FAILURE;
        }
        log::Info("logger first-path marker");
        if (!Expect(log::Initialize(path, true),
                    "same-path logger reinitialization should be idempotent")) {
            return EXIT_FAILURE;
        }
        log::Info("logger same-path marker");
        if (!Expect(log::Initialize(second_path, true),
                    "logger should switch paths without an explicit shutdown")) {
            return EXIT_FAILURE;
        }
        log::Info("logger second-path marker");
        log::Shutdown();

        std::ifstream first_stream(path, std::ios::binary);
        const std::string first_contents(
            (std::istreambuf_iterator<char>(first_stream)), {});
        std::ifstream second_stream(second_path, std::ios::binary);
        const std::string second_contents(
            (std::istreambuf_iterator<char>(second_stream)), {});
        if (!Expect(
                first_contents.find("logger first-path marker") !=
                        std::string::npos &&
                    first_contents.find("logger same-path marker") !=
                        std::string::npos &&
                    first_contents.find("logger second-path marker") ==
                        std::string::npos &&
                    second_contents.find("logger first-path marker") ==
                        std::string::npos &&
                    second_contents.find("logger same-path marker") ==
                        std::string::npos &&
                    second_contents.find("logger second-path marker") !=
                        std::string::npos,
                "logger reinitialization should preserve same-path writes and "
                "route subsequent writes only to the requested new path")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(second_path);
    }

    {
        const auto path = MakeTempIniPath(L"spatch-config-defaults-test.ini");
        RemoveIfExists(path);

        const Config config = LoadConfig(path);
        if (!Expect(std::filesystem::exists(path),
                    "LoadConfig should create missing ini")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.fix_cutscene_zero_dt,
                    "default cutscene zero-dt fix should be enabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.fix_cutscene_scene_time_step,
                    "default cutscene scene-time fix should be enabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!config.prefer_max_refresh_rate,
                    "default max-refresh preference should be disabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!config.hook_nis_actor_state,
                    "default NIS actor-state hook should be disabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!config.hook_twitch_probe,
                    "default Twitch probe hook should be disabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!config.fix_nis_actor_restore_duplicates,
                    "default NIS duplicate-restore fix should be disabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.cutscene_fps == 0,
                    "default cutscene cadence should follow the live rate")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.write_minidumps,
                    "crash dumps should be enabled by default")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.summary_interval_ms == 0 &&
                        config.max_verbose_events == 0 &&
                        config.max_unique_callbacks == 0 &&
                        !config.enable_logging,
                    "default diagnostics should be disabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.override_fullscreen == -1,
                    "default fullscreen override should keep the game value")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                config.override_shadow_filter == -1 &&
                    config.override_texture_detail_level == -1 &&
                    config.override_motion_blur == -1,
                "fresh installs should preserve the user's graphics-quality "
                "settings")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.anisotropic_filtering == 16 &&
                        config.force_anisotropic_filtering,
                    "texture filtering should default to the documented "
                    "16x narrow-promotion policy")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.smaa_preset == 3,
                    "SMAA should default to the canonical Ultra preset")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.fix_pedestrian_density_at_high_fps &&
                        config.improve_spherical_reflections &&
                        config.restore_original_fog &&
                        config.restore_original_eye_reflections &&
                        config.restore_character_wetness &&
                        config.restore_character_sweat &&
                        config.wetness_full_time_seconds == 30 &&
                        config.wetness_fade_time_seconds == 270 &&
                        config.sweat_build_time_seconds == 150 &&
                        config.sweat_fade_time_seconds == 120 &&
                        config.sweat_onset_time_seconds == 30 &&
                        std::fabs(config.sweat_run_speed - 2.5f) < 0.0001f &&
                        config.sweat_combat_time_seconds == 15 &&
                        config.remove_hidden_120_fps_cap &&
                        config.fix_first_run_resolution &&
                        config.fix_scaleform_qpc_clock &&
                        config.fix_file_timestamp_open_mode &&
                        config.fix_audio_file_open &&
                        config.fix_large_file_sizes &&
                        config.fix_vram_pool_lock &&
                        config.fix_vram_capacity_reporting &&
                        config.fix_resource_loading &&
                        config.fix_contact_list_overflow &&
                        config.fix_corrupt_save_handling &&
                        config.fix_thread_creation_failure &&
                        config.fix_high_fps_average_windows &&
                        config.spherical_reflection_width == 0 &&
                        config.force_raw_mouse_input &&
                        config.disable_camera_smoothing &&
                        !config.gta_iv_car_camera &&
                        !config.gta_iv_bike_camera &&
                        config.controller_left_stick_deadzone == -1 &&
                        config.controller_right_stick_deadzone == -1,
                    "new executable fixes should use safe end-user defaults")) {
            return EXIT_FAILURE;
        }
        std::ifstream ini(path, std::ios::binary);
        const std::string ini_text((std::istreambuf_iterator<char>(ini)), {});
        constexpr std::array migrated_graphics_tokens{
            "[Tonemapping]", "AgX=", "ShadowResolution", "[AmbientOcclusion]",
            "[GlobalIllumination]", "[PhysicallyBasedRendering]",
            "[SubsurfaceScattering]", "[MaterialScattering]", "DumpShaders",
            "CensusShadowConsumers"};
        bool found_migrated_graphics_token = false;
        for (const char* token : migrated_graphics_tokens) {
            found_migrated_graphics_token |= ini_text.find(token) != std::string::npos;
        }
        constexpr std::string_view expected_tail =
            "; Write a small diagnostic dump if the game crashes.\n"
            "WriteCrashDumps=1";
        const bool exact_tail =
            ini_text.size() >= expected_tail.size() &&
            ini_text.compare(ini_text.size() - expected_tail.size(),
                             expected_tail.size(), expected_tail) == 0;
        const std::size_t debug_section = ini_text.find("[Debug]");
        const std::size_t logging_key = ini_text.find("Logging=0", debug_section);
        const std::size_t crash_comment = ini_text.find(
            "; Write a small diagnostic dump if the game crashes.", logging_key);
        const std::size_t crash_key = ini_text.find("WriteCrashDumps=1", crash_comment);
        const std::size_t input_section = ini_text.find("[Input]");
        const std::size_t gta_iv_camera_comment = ini_text.find(
            "; GTA IV-like camera for exact road-vehicle Drive/Flee paths, including trucks and buses.",
            input_section);
        const std::size_t gta_iv_camera_dynamics = ini_text.find(
            "; Adds a right-seat offset, loose follow, manual yaw/pitch, delayed recentering, and handbrake swing.",
            gta_iv_camera_comment);
        const std::size_t gta_iv_camera_exclusions = ini_text.find(
            "; Race, Hijack, Aim, Look, reverse, and special override paths stay stock.",
            gta_iv_camera_dynamics);
        const std::size_t gta_iv_camera_restart = ini_text.find(
            "; 1 enables and 0 disables. Changing this requires a restart.",
            gta_iv_camera_exclusions);
        const std::size_t gta_iv_camera_key =
            ini_text.find("GTAIVCarCamera=0", gta_iv_camera_restart);
        const std::size_t gta_iv_bike_camera_comment = ini_text.find(
            "; Apply the same GTA IV-like behavior independently to motorcycle/scooter Drive cameras.",
            gta_iv_camera_key);
        const std::size_t gta_iv_bike_camera_aliases = ini_text.find(
            "; The exact Drive branch must be active even when Race or HijackFront aliases its block.",
            gta_iv_bike_camera_comment);
        const std::size_t gta_iv_bike_camera_exclusions = ini_text.find(
            "; Distinct Aim, Flee, look, passenger, and hijack camera blocks remain stock.",
            gta_iv_bike_camera_aliases);
        const std::size_t gta_iv_bike_camera_restart = ini_text.find(
            "; 1 enables and 0 disables. Changing this requires a restart.",
            gta_iv_bike_camera_exclusions);
        const std::size_t gta_iv_bike_camera_key =
            ini_text.find("GTAIVBikeCamera=0", gta_iv_bike_camera_restart);
        const std::size_t graphics_section =
            ini_text.find("[Graphics]", gta_iv_bike_camera_key);
        if (!Expect(
                ini_text.find("ConfigVersion=44") != std::string::npos &&
                    ini_text.find("OriginalShadowFilter=-1") !=
                        std::string::npos &&
                    ini_text.find("[TextureFiltering]") != std::string::npos &&
                    ini_text.find("AnisotropicFiltering=16") !=
                        std::string::npos &&
                    ini_text.find("ForceAnisotropicFiltering=1") !=
                        std::string::npos &&
                    input_section < gta_iv_camera_comment &&
                    gta_iv_camera_comment < gta_iv_camera_dynamics &&
                    gta_iv_camera_dynamics < gta_iv_camera_exclusions &&
                    gta_iv_camera_exclusions < gta_iv_camera_restart &&
                    gta_iv_camera_restart < gta_iv_camera_key &&
                    gta_iv_camera_key < gta_iv_bike_camera_comment &&
                    gta_iv_bike_camera_comment <
                        gta_iv_bike_camera_aliases &&
                    gta_iv_bike_camera_aliases <
                        gta_iv_bike_camera_exclusions &&
                    gta_iv_bike_camera_exclusions <
                        gta_iv_bike_camera_restart &&
                    gta_iv_bike_camera_restart < gta_iv_bike_camera_key &&
                    gta_iv_bike_camera_key < graphics_section &&
                    ini_text.find("GTAIVCarCamera=", gta_iv_camera_key + 1) ==
                        std::string::npos &&
                    ini_text.find("GTAIVBikeCamera=",
                                  gta_iv_bike_camera_key + 1) ==
                        std::string::npos &&
                    ini_text.find("gta_iv_car_camera=") ==
                        std::string::npos &&
                    ini_text.find("gta_iv_bike_camera=") ==
                        std::string::npos &&
                    !found_migrated_graphics_token && exact_tail &&
                    debug_section < logging_key && logging_key < crash_comment &&
                    crash_comment < crash_key &&
                    !std::filesystem::exists(ConfigBackupPath(path)) &&
                    !std::filesystem::exists(
                        InGameLegacyConfigBackupPath(path)),
                "v44 defaults should expose independent default-off GTA IV "
                "car and bike cameras, keep "
                "native texture filtering, exclude ShenLong rendering controls, "
                "and end exactly with the crash dump switch")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    for (const auto [car_value, bike_value] :
         std::array{std::pair{0, 0}, std::pair{0, 1}, std::pair{1, 0},
                    std::pair{1, 1}}) {
        const auto path = GetTestTempRoot() /
                          (L"spatch-v44-gtaiv-camera-combination-" +
                           std::to_wstring(car_value) + L"-" +
                           std::to_wstring(bike_value) + L".ini");
        RemoveIfExists(path);
        const std::string source_text =
            "[SPatch]\nConfigVersion=44\n"
            "[Input]\nGTAIVCarCamera=" +
            std::to_string(car_value) + "\nGTAIVBikeCamera=" +
            std::to_string(bike_value) + "\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config parsed = LoadConfig(path, &report);
        std::ifstream parsed_stream(path, std::ios::binary);
        const std::string parsed_text(
            (std::istreambuf_iterator<char>(parsed_stream)), {});
        if (!Expect(
                parsed.gta_iv_car_camera == (car_value != 0) &&
                    parsed.gta_iv_bike_camera == (bike_value != 0) &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    parsed_text == source_text &&
                    !std::filesystem::exists(ConfigBackupPath(path)),
                "all four canonical car/bike camera combinations should parse "
                "independently without rewriting a current config")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        constexpr std::array<std::string_view, 12> malformed_values{
            "",   "-1",    "2",   "01",  "+1", "true",
            "false", "on", "off", "yes", "no", "1junk"};
        for (std::size_t index = 0; index < malformed_values.size(); ++index) {
            const auto path = GetTestTempRoot() /
                              (L"spatch-v44-gtaiv-bike-camera-malformed-" +
                               std::to_wstring(index) + L".ini");
            RemoveIfExists(path);
            const std::string source_text =
                "[SPatch]\nConfigVersion=44\n"
                "[Input]\nGTAIVCarCamera=1\nGTAIVBikeCamera=" +
                std::string(malformed_values[index]) + "\n";
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config parsed = LoadConfig(path, &report);
            std::ifstream parsed_stream(path, std::ios::binary);
            const std::string parsed_text(
                (std::istreambuf_iterator<char>(parsed_stream)), {});
            if (!Expect(
                    parsed.gta_iv_car_camera &&
                        !parsed.gta_iv_bike_camera &&
                        report.source_version == kConfigVersion &&
                        report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        parsed_text == source_text,
                    "GTAIVBikeCamera should accept only literal 0 and 1, fail "
                    "closed independently, and leave a current file untouched")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
        }
    }

    {
        constexpr std::array<std::string_view, 2> trimmed_values{
            " 0 \t", "\t1 "};
        for (std::size_t index = 0; index < trimmed_values.size(); ++index) {
            const auto path = GetTestTempRoot() /
                              (L"spatch-v44-gtaiv-bike-camera-trimmed-" +
                               std::to_wstring(index) + L".ini");
            RemoveIfExists(path);
            const std::string source_text =
                "[SPatch]\nConfigVersion=44\n"
                "[Input]\nGTAIVBikeCamera=" +
                std::string(trimmed_values[index]) + "\n";
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config parsed = LoadConfig(path, &report);
            std::ifstream parsed_stream(path, std::ios::binary);
            const std::string parsed_text(
                (std::istreambuf_iterator<char>(parsed_stream)), {});
            if (!Expect(
                    parsed.gta_iv_bike_camera == (index != 0) &&
                        report.source_version == kConfigVersion &&
                        report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        parsed_text == source_text,
                    "GTAIVBikeCamera should accept trimmed literal 0 and 1 "
                    "without rewriting a current config")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
        }
    }

    {
        constexpr std::array<std::string_view, 7> malformed_values{
            "", "-1", "2", "true", "on", "yes", "1junk"};
        for (std::size_t index = 0; index < malformed_values.size(); ++index) {
            const auto path = GetTestTempRoot() /
                              (L"spatch-v44-gtaiv-car-camera-malformed-" +
                               std::to_wstring(index) + L".ini");
            RemoveIfExists(path);
            const std::string source_text =
                "[SPatch]\nConfigVersion=44\n"
                "[Input]\nGTAIVCarCamera=" +
                std::string(malformed_values[index]) + "\n";
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config parsed = LoadConfig(path, &report);
            std::ifstream parsed_stream(path, std::ios::binary);
            const std::string parsed_text(
                (std::istreambuf_iterator<char>(parsed_stream)), {});
            if (!Expect(
                    !parsed.gta_iv_car_camera &&
                        report.source_version == kConfigVersion &&
                        report.persistence == ConfigPersistenceStatus::Unchanged &&
                        parsed_text == source_text,
                    "GTAIVCarCamera should accept only literal 0 and 1 and "
                    "otherwise use its safe disabled default")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
        }
    }

    {
        struct GtaIvCarCameraAliasCase {
            const wchar_t* filename;
            const char* settings;
            bool expected;
        };
        constexpr std::array<GtaIvCarCameraAliasCase, 3> cases{{
            {L"spatch-v42-gtaiv-car-camera-alias-migration.ini",
             "[Input]\n"
             "gta_iv_car_camera=1\n",
             true},
            {L"spatch-v42-gtaiv-car-camera-canonical-precedence.ini",
             "gta_iv_car_camera=1\n"
             "[Input]\n"
             "GTAIVCarCamera=0\n"
             "gta_iv_car_camera=1\n",
             false},
            {L"spatch-v42-gtaiv-car-camera-empty-canonical-precedence.ini",
             "gta_iv_car_camera=1\n"
             "[Input]\n"
             "GTAIVCarCamera=\n",
             false},
        }};

        for (const GtaIvCarCameraAliasCase& test_case : cases) {
            const auto path = MakeTempIniPath(test_case.filename);
            const auto backup = ConfigBackupPath(path);
            RemoveIfExists(path);
            RemoveIfExists(backup);
            const std::string source_text =
                "[SPatch]\nConfigVersion=42\n" +
                std::string(test_case.settings);
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config migrated = LoadConfig(path, &report);
            std::ifstream migrated_stream(path, std::ios::binary);
            const std::string migrated_text(
                (std::istreambuf_iterator<char>(migrated_stream)), {});
            std::ifstream backup_stream(backup, std::ios::binary);
            const std::string backup_text(
                (std::istreambuf_iterator<char>(backup_stream)), {});
            ConfigLoadReport reload_report{};
            const Config reloaded = LoadConfig(path, &reload_report);
            if (!Expect(
                    migrated.gta_iv_car_camera == test_case.expected &&
                        reloaded.gta_iv_car_camera == test_case.expected &&
                        report.source_version == 42 &&
                        report.persistence == ConfigPersistenceStatus::Migrated &&
                        reload_report.source_version == kConfigVersion &&
                        reload_report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        backup_text == source_text &&
                        migrated_text.find(
                            std::string("GTAIVCarCamera=") +
                            (test_case.expected ? "1" : "0")) !=
                            std::string::npos &&
                        migrated_text.find("gta_iv_car_camera=") ==
                            std::string::npos &&
                        !HasInGameConfigWorkResidue(path),
                    "v42 GTA IV car-camera aliases should migrate once, with "
                    "the canonical Input key taking precedence")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
            RemoveIfExists(backup);
        }
    }

    {
        struct GtaIvBikeCameraMigrationCase {
            const wchar_t* filename;
            const char* settings;
            bool expected;
        };
        constexpr std::array<GtaIvBikeCameraMigrationCase, 13> cases{{
            {L"spatch-v43-gtaiv-bike-camera-absent.ini", "", false},
            {L"spatch-v43-gtaiv-bike-camera-spatch-canonical-0.ini",
             "GTAIVBikeCamera=0\n", false},
            {L"spatch-v43-gtaiv-bike-camera-spatch-canonical-1.ini",
             "GTAIVBikeCamera=1\n", true},
            {L"spatch-v43-gtaiv-bike-camera-spatch-snake-0.ini",
             "gta_iv_bike_camera=0\n", false},
            {L"spatch-v43-gtaiv-bike-camera-spatch-snake-1.ini",
             "gta_iv_bike_camera=1\n", true},
            {L"spatch-v43-gtaiv-bike-camera-canonical-0.ini",
             "[Input]\nGTAIVBikeCamera=0\n", false},
            {L"spatch-v43-gtaiv-bike-camera-canonical-1.ini",
             "[Input]\nGTAIVBikeCamera=1\n", true},
            {L"spatch-v43-gtaiv-bike-camera-snake-0.ini",
             "[Input]\ngta_iv_bike_camera=0\n", false},
            {L"spatch-v43-gtaiv-bike-camera-snake-1.ini",
             "[Input]\ngta_iv_bike_camera=1\n", true},
            {L"spatch-v43-gtaiv-bike-camera-canonical-precedence.ini",
             "GTAIVBikeCamera=1\n"
             "gta_iv_bike_camera=1\n"
             "[Input]\n"
             "GTAIVBikeCamera=0\n"
             "gta_iv_bike_camera=1\n",
             false},
            {L"spatch-v43-gtaiv-bike-camera-malformed-precedence.ini",
             "gta_iv_bike_camera=1\n"
             "[Input]\n"
             "GTAIVBikeCamera=true\n"
             "gta_iv_bike_camera=1\n",
             false},
            {L"spatch-v43-gtaiv-bike-camera-empty-precedence.ini",
             "gta_iv_bike_camera=1\n"
             "[Input]\n"
             "GTAIVBikeCamera=\n"
             "gta_iv_bike_camera=1\n",
             false},
            {L"spatch-v43-gtaiv-bike-camera-near-aliases.ini",
             "[Input]\n"
             "gtaiv_bike_camera=1\n"
             "gta_iv_motorcycle_camera=1\n",
             false},
        }};

        for (const GtaIvBikeCameraMigrationCase& test_case : cases) {
            const auto path = MakeTempIniPath(test_case.filename);
            const auto backup = ConfigBackupPath(path);
            RemoveIfExists(path);
            RemoveIfExists(backup);
            const std::string source_text =
                "[SPatch]\nConfigVersion=43\nGTAIVCarCamera=1\n" +
                std::string(test_case.settings);
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config migrated = LoadConfig(path, &report);
            std::ifstream migrated_stream(path, std::ios::binary);
            const std::string migrated_text(
                (std::istreambuf_iterator<char>(migrated_stream)), {});
            std::ifstream backup_stream(backup, std::ios::binary);
            const std::string backup_text(
                (std::istreambuf_iterator<char>(backup_stream)), {});
            ConfigLoadReport reload_report{};
            const Config reloaded = LoadConfig(path, &reload_report);
            std::ifstream reloaded_stream(path, std::ios::binary);
            const std::string reloaded_text(
                (std::istreambuf_iterator<char>(reloaded_stream)), {});
            const std::size_t canonical_car =
                migrated_text.find("GTAIVCarCamera=1");
            const std::size_t canonical_bike = migrated_text.find(
                std::string("GTAIVBikeCamera=") +
                (test_case.expected ? "1" : "0"));
            if (!Expect(
                    migrated.gta_iv_car_camera &&
                        reloaded.gta_iv_car_camera &&
                        migrated.gta_iv_bike_camera == test_case.expected &&
                        reloaded.gta_iv_bike_camera == test_case.expected &&
                        report.source_version == 43 &&
                        report.persistence ==
                            ConfigPersistenceStatus::Migrated &&
                        reload_report.source_version == kConfigVersion &&
                        reload_report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        backup.filename() == L"SPatch-pre-v44.ini" &&
                        backup_text == source_text &&
                        reloaded_text == migrated_text &&
                        migrated_text.find("ConfigVersion=44") !=
                            std::string::npos &&
                        canonical_car != std::string::npos &&
                        migrated_text.find("GTAIVCarCamera=",
                                           canonical_car + 1) ==
                            std::string::npos &&
                        canonical_bike != std::string::npos &&
                        migrated_text.find("GTAIVBikeCamera=",
                                           canonical_bike + 1) ==
                            std::string::npos &&
                        migrated_text.find("gta_iv_car_camera=") ==
                            std::string::npos &&
                        migrated_text.find("gta_iv_bike_camera=") ==
                            std::string::npos &&
                        migrated_text.ends_with(
                            "; Write a small diagnostic dump if the game crashes.\n"
                            "WriteCrashDumps=1") &&
                        !HasInGameConfigWorkResidue(path),
                    "v43 GTA IV bike-camera absence, canonical values, sole "
                    "snake alias, and canonical Input precedence should "
                    "migrate once to v44 without alias leakage")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
            RemoveIfExists(backup);
        }
    }

    {
        const auto path = MakeTempIniPath(
            L"spatch-v44-gtaiv-car-camera-regeneration.ini");
        RemoveIfExists(path);
        if (!Expect(WriteBoolValue(path, L"gta_iv_car_camera", true),
                    "the GTA IV car-camera persistence helper should regenerate "
                    "a missing canonical config")) {
            return EXIT_FAILURE;
        }

        ConfigLoadReport report{};
        const Config reloaded = LoadConfig(path, &report);
        std::ifstream stream(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), {});
        const std::size_t canonical = text.find("GTAIVCarCamera=1");
        const std::size_t crash_dump_key = text.rfind("WriteCrashDumps=1");
        if (!Expect(
                reloaded.gta_iv_car_camera &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    canonical != std::string::npos &&
                    text.find("GTAIVCarCamera=", canonical + 1) ==
                        std::string::npos &&
                    text.find("gta_iv_car_camera=") == std::string::npos &&
                    crash_dump_key != std::string::npos &&
                    text.find_first_not_of(
                        "\r\n", crash_dump_key + std::string_view(
                                                     "WriteCrashDumps=1")
                                                     .size()) ==
                        std::string::npos &&
                    !std::filesystem::exists(ConfigBackupPath(path)),
                "regeneration should write only the canonical Input key and "
                "reload the requested value with WriteCrashDumps as the final "
                "key")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path = MakeTempIniPath(
            L"spatch-v44-gtaiv-bike-camera-regeneration.ini");
        RemoveIfExists(path);
        if (!Expect(WriteBoolValue(path, L"gta_iv_bike_camera", true),
                    "the GTA IV bike-camera persistence helper should "
                    "regenerate a missing canonical config")) {
            return EXIT_FAILURE;
        }

        ConfigLoadReport report{};
        const Config reloaded = LoadConfig(path, &report);
        std::ifstream stream(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), {});
        const std::size_t canonical = text.find("GTAIVBikeCamera=1");
        const std::size_t crash_dump_key = text.rfind("WriteCrashDumps=1");
        if (!Expect(
                reloaded.gta_iv_bike_camera &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    canonical != std::string::npos &&
                    text.find("GTAIVBikeCamera=", canonical + 1) ==
                        std::string::npos &&
                    text.find("gta_iv_bike_camera=") == std::string::npos &&
                    crash_dump_key != std::string::npos &&
                    text.find_first_not_of(
                        "\r\n", crash_dump_key + std::string_view(
                                                     "WriteCrashDumps=1")
                                                     .size()) ==
                        std::string::npos &&
                    !std::filesystem::exists(ConfigBackupPath(path)),
                "bike-camera regeneration should write only the canonical "
                "Input key, reload true, and retain WriteCrashDumps as the "
                "literal final key")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        struct MigratedGraphicsKeyCase {
            const char* section;
            const char* key;
        };
        constexpr std::array cases{
            MigratedGraphicsKeyCase{"Tonemapping", "AgX"},
            MigratedGraphicsKeyCase{"Tonemapping", "AgXLook"},
            MigratedGraphicsKeyCase{"Tonemapping", "AgXStrength"},
            MigratedGraphicsKeyCase{"Tonemapping", "AgXExposure"},
            MigratedGraphicsKeyCase{"Shadows", "ShadowResolution"},
            MigratedGraphicsKeyCase{"AmbientOcclusion", "AmbientOcclusion"},
            MigratedGraphicsKeyCase{"AmbientOcclusion", "SDAOQuality"},
            MigratedGraphicsKeyCase{"GlobalIllumination", "GlobalIllumination"},
            MigratedGraphicsKeyCase{"PhysicallyBasedRendering", "PhysicallyBasedRendering"},
            MigratedGraphicsKeyCase{"SubsurfaceScattering", "SubsurfaceScattering"},
            MigratedGraphicsKeyCase{"SubsurfaceScattering", "StockHairBlur"},
            MigratedGraphicsKeyCase{"MaterialScattering", "WaterScattering"},
            MigratedGraphicsKeyCase{"MaterialScattering", "WaterVolumetricScattering"},
            MigratedGraphicsKeyCase{"MaterialScattering", "VolumetricScattering"},
            MigratedGraphicsKeyCase{"MaterialScattering", "water_sss"},
            MigratedGraphicsKeyCase{"MaterialScattering", "WaterScatteringStrength"},
            MigratedGraphicsKeyCase{"MaterialScattering", "ScatteringStrength"},
            MigratedGraphicsKeyCase{"MaterialScattering", "WaterScatteringAnisotropy"},
            MigratedGraphicsKeyCase{"MaterialScattering", "ScatteringAnisotropy"},
            MigratedGraphicsKeyCase{"Debug", "DumpShaders"},
            MigratedGraphicsKeyCase{"Debug", "CensusShadowConsumers"},
        };

        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto path = GetTestTempRoot() /
                              (L"spatch-v42-migrated-graphics-key-" +
                               std::to_wstring(index) + L".ini");
            const auto backup = ConfigBackupPath(path);
            RemoveIfExists(path);
            RemoveIfExists(backup);
            const std::string source_text =
                "[SPatch]\r\nConfigVersion=44\r\nEnabled=0\r\n[" +
                std::string(cases[index].section) + "]\r\n" +
                cases[index].key + "=\r\n";
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config migrated = LoadConfig(path, &report);
            std::ifstream backup_stream(backup, std::ios::binary);
            const std::string backup_text(
                (std::istreambuf_iterator<char>(backup_stream)), {});
            std::ifstream migrated_stream(path, std::ios::binary);
            const std::string migrated_text(
                (std::istreambuf_iterator<char>(migrated_stream)), {});
            ConfigLoadReport reload_report{};
            (void)LoadConfig(path, &reload_report);
            if (!Expect(!migrated.enabled &&
                            report.source_version == kConfigVersion &&
                            report.persistence == ConfigPersistenceStatus::Migrated &&
                            backup_text == source_text &&
                            migrated_text.find(cases[index].key) == std::string::npos &&
                            !std::filesystem::exists(
                                InGameLegacyConfigBackupPath(path)) &&
                            reload_report.persistence ==
                                ConfigPersistenceStatus::Unchanged,
                        "every exact migrated ShenLong key should preserve the source bytes "
                        "outside the game directory and be removed by the v42 rewrite")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
            RemoveIfExists(backup);
        }

        const auto near_match_path =
            MakeTempIniPath(L"spatch-v42-migrated-graphics-near-match.ini");
        const auto near_match_backup = ConfigBackupPath(near_match_path);
        RemoveIfExists(near_match_path);
        RemoveIfExists(near_match_backup);
        const std::string near_match_text =
            "[SPatch]\nConfigVersion=44\nEnabled=1\n"
            "[MaterialScattering]\nWaterScatteringSuffix=1\n";
        {
            std::ofstream stream(
                near_match_path, std::ios::binary | std::ios::trunc);
            stream << near_match_text;
        }
        ConfigLoadReport near_match_report{};
        (void)LoadConfig(near_match_path, &near_match_report);
        std::ifstream near_match_stream(near_match_path, std::ios::binary);
        const std::string near_match_after(
            (std::istreambuf_iterator<char>(near_match_stream)), {});
        if (!Expect(near_match_report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        near_match_after == near_match_text &&
                        !std::filesystem::exists(near_match_backup) &&
                        !std::filesystem::exists(
                            InGameLegacyConfigBackupPath(near_match_path)),
                    "migrated-key detection should require an exact key name")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(near_match_path);
        RemoveIfExists(near_match_backup);
    }

    for (const int value : std::array{-1, 4, 8, 16}) {
        const auto path = GetTestTempRoot() /
                          (L"spatch-v42-anisotropic-allowed-" +
                           std::to_wstring(value) + L".ini");
        RemoveIfExists(path);
        const std::string source_text =
            "[SPatch]\nConfigVersion=44\n"
            "[TextureFiltering]\nAnisotropicFiltering=" +
            std::to_string(value) +
            "\nForceAnisotropicFiltering=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config parsed = LoadConfig(path, &report);
        std::ifstream parsed_stream(path, std::ios::binary);
        const std::string parsed_text(
            (std::istreambuf_iterator<char>(parsed_stream)), {});
        if (!Expect(
                parsed.anisotropic_filtering == value &&
                    !parsed.force_anisotropic_filtering &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    parsed_text == source_text &&
                    !std::filesystem::exists(ConfigBackupPath(path)) &&
                    !std::filesystem::exists(
                        InGameLegacyConfigBackupPath(path)),
                "all documented anisotropic levels should parse without "
                "rewriting a current canonical config")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    for (const std::string_view value :
         std::array<std::string_view, 5>{"-2", "0", "2", "32", "invalid"}) {
        const auto path = GetTestTempRoot() /
                          (L"spatch-v42-anisotropic-invalid-" +
                           std::to_wstring(value.front()) + L"-" +
                           std::to_wstring(value.size()) + L".ini");
        RemoveIfExists(path);
        const std::string source_text =
            "[SPatch]\nConfigVersion=44\n"
            "[TextureFiltering]\nAnisotropicFiltering=" +
            std::string(value) + "\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config parsed = LoadConfig(path, &report);
        std::ifstream parsed_stream(path, std::ios::binary);
        const std::string parsed_text(
            (std::istreambuf_iterator<char>(parsed_stream)), {});
        if (!Expect(
                parsed.anisotropic_filtering == -1 &&
                    parsed.force_anisotropic_filtering &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    parsed_text == source_text,
                "a present unsupported anisotropic value should fail closed "
                "to the native game setting")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        struct TextureAliasCase {
            const char* section;
            const char* anisotropic_key;
            const char* anisotropic_value;
            int expected_anisotropic;
            const char* force_key;
            const char* force_value;
            bool expected_force;
        };
        constexpr std::array cases{
            TextureAliasCase{"Graphics", "AnisotropicFiltering", "4", 4,
                             "ForceAnisotropicFiltering", "0", false},
            TextureAliasCase{"SPatch", "AnisotropicFiltering", "8", 8,
                             "ForceAnisotropicFiltering", "1", true},
            TextureAliasCase{"TextureFiltering", "anisotropic_filtering", "16", 16,
                             "force_anisotropic_filtering", "0", false},
            TextureAliasCase{"Graphics", "anisotropic_filtering", "-1", -1,
                             "force_anisotropic_filtering", "1", true},
            TextureAliasCase{"SPatch", "anisotropic_filtering", "4", 4,
                             "force_anisotropic_filtering", "0", false},
        };

        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto path = GetTestTempRoot() /
                              (L"spatch-v42-texture-alias-" +
                               std::to_wstring(index) + L".ini");
            const auto backup = ConfigBackupPath(path);
            RemoveIfExists(path);
            RemoveIfExists(backup);
            const TextureAliasCase& test_case = cases[index];
            std::string source_text = "[SPatch]\nConfigVersion=44\n";
            if (std::string_view(test_case.section) != "SPatch") {
                source_text += "[" + std::string(test_case.section) + "]\n";
            }
            source_text += std::string(test_case.anisotropic_key) + "=" +
                           test_case.anisotropic_value + "\n" +
                           test_case.force_key + "=" + test_case.force_value +
                           "\n";
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            const Config parsed = LoadConfig(path, &report);
            std::ifstream parsed_stream(path, std::ios::binary);
            const std::string parsed_text(
                (std::istreambuf_iterator<char>(parsed_stream)), {});
            std::ifstream backup_stream(backup, std::ios::binary);
            const std::string backup_text(
                (std::istreambuf_iterator<char>(backup_stream)), {});
            ConfigLoadReport reload_report{};
            const Config reloaded = LoadConfig(path, &reload_report);
            if (!Expect(
                    parsed.anisotropic_filtering ==
                            test_case.expected_anisotropic &&
                        parsed.force_anisotropic_filtering ==
                            test_case.expected_force &&
                        reloaded.anisotropic_filtering ==
                            test_case.expected_anisotropic &&
                        reloaded.force_anisotropic_filtering ==
                            test_case.expected_force &&
                        report.persistence ==
                            ConfigPersistenceStatus::Migrated &&
                        reload_report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        backup_text == source_text &&
                        parsed_text.find("[TextureFiltering]") !=
                            std::string::npos &&
                        parsed_text.find(
                            "AnisotropicFiltering=" +
                            std::to_string(test_case.expected_anisotropic)) !=
                            std::string::npos &&
                        parsed_text.find(
                            std::string("ForceAnisotropicFiltering=") +
                            (test_case.expected_force ? "1" : "0")) !=
                            std::string::npos &&
                        parsed_text.find("anisotropic_filtering=") ==
                            std::string::npos &&
                        parsed_text.find("force_anisotropic_filtering=") ==
                            std::string::npos &&
                        !HasInGameConfigWorkResidue(path),
                    "legacy texture-filtering sections and snake-case aliases "
                    "should migrate once to canonical SPatch keys with an exact "
                    "external backup")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
            RemoveIfExists(backup);
        }
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v42-texture-canonical-precedence.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=44\n"
            "AnisotropicFiltering=4\n"
            "ForceAnisotropicFiltering=0\n"
            "anisotropic_filtering=8\n"
            "force_anisotropic_filtering=0\n"
            "[Graphics]\n"
            "AnisotropicFiltering=8\n"
            "ForceAnisotropicFiltering=0\n"
            "anisotropic_filtering=4\n"
            "force_anisotropic_filtering=0\n"
            "[TextureFiltering]\n"
            "AnisotropicFiltering=16\n"
            "ForceAnisotropicFiltering=1\n"
            "anisotropic_filtering=-1\n"
            "force_anisotropic_filtering=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config parsed = LoadConfig(path, &report);
        std::ifstream parsed_stream(path, std::ios::binary);
        const std::string parsed_text(
            (std::istreambuf_iterator<char>(parsed_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        ConfigLoadReport reload_report{};
        const Config reloaded = LoadConfig(path, &reload_report);
        if (!Expect(
                parsed.anisotropic_filtering == 16 &&
                    parsed.force_anisotropic_filtering &&
                    reloaded.anisotropic_filtering == 16 &&
                    reloaded.force_anisotropic_filtering &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    backup_text == source_text &&
                    parsed_text.find("AnisotropicFiltering=16") !=
                        std::string::npos &&
                    parsed_text.find("ForceAnisotropicFiltering=1") !=
                        std::string::npos &&
                    parsed_text.find("anisotropic_filtering=") ==
                        std::string::npos &&
                    parsed_text.find("force_anisotropic_filtering=") ==
                        std::string::npos &&
                    !HasInGameConfigWorkResidue(path),
                "canonical TextureFiltering spellings should win over every "
                "legacy section and snake-case alias during one backed migration")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        struct RetiredRendererCase {
            const wchar_t* filename;
            int source_version;
            const char* settings;
        };
        constexpr std::array<RetiredRendererCase, 2> cases{{
            {L"spatch-v35-retired-renderer-migration-test.ini",
             35,
             "SwapChainTearing=1\n"
             "swap_chain_frame_latency=3\n"
             "[Graphics]\n"
             "RendererBackend=dxvk\n"
             "swap_chain_flip_model=1\n"},
            {L"spatch-v42-retired-renderer-migration-test.ini",
             kConfigVersion,
             "RendererBackend=dx12\n"
             "SwapChainFlipModel=1\n"
             "[Renderer]\n"
             "RendererBackend=dxvk\n"
             "SwapChainTearing=1\n"
             "SwapChainFrameLatency=2\n"},
        }};

        for (const RetiredRendererCase& test_case : cases) {
            const auto path = MakeTempIniPath(test_case.filename);
            const auto backup = ConfigBackupPath(path);
            RemoveIfExists(path);
            RemoveIfExists(backup);
            const std::string source_text =
                "[SPatch]\nConfigVersion=" +
                std::to_string(test_case.source_version) + "\n" +
                test_case.settings;
            {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << source_text;
            }

            ConfigLoadReport report{};
            (void)LoadConfig(path, &report);
            std::ifstream migrated_stream(path, std::ios::binary);
            const std::string migrated_text(
                (std::istreambuf_iterator<char>(migrated_stream)), {});
            std::ifstream backup_stream(backup, std::ios::binary);
            const std::string backup_text(
                (std::istreambuf_iterator<char>(backup_stream)), {});
            ConfigLoadReport reload_report{};
            (void)LoadConfig(path, &reload_report);
            std::ifstream reloaded_stream(path, std::ios::binary);
            const std::string reloaded_text(
                (std::istreambuf_iterator<char>(reloaded_stream)), {});
            if (!Expect(
                    report.source_version == test_case.source_version &&
                        report.persistence ==
                            ConfigPersistenceStatus::Migrated &&
                        reload_report.source_version == kConfigVersion &&
                        reload_report.persistence ==
                            ConfigPersistenceStatus::Unchanged &&
                        migrated_text.find("ConfigVersion=44") !=
                            std::string::npos &&
                        migrated_text.find("[Renderer]") ==
                            std::string::npos &&
                        migrated_text.find("RendererBackend=") ==
                            std::string::npos &&
                        migrated_text.find("renderer_backend=") ==
                            std::string::npos &&
                        migrated_text.find("SwapChain") ==
                            std::string::npos &&
                        migrated_text.find("swap_chain_") ==
                            std::string::npos &&
                        backup_text == source_text &&
                        reloaded_text == migrated_text,
                    "retired renderer and swap-chain settings should migrate "
                    "once to the stock native D3D11 contract")) {
                return EXIT_FAILURE;
            }
            RemoveIfExists(path);
            RemoveIfExists(backup);
        }
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v42-retired-unverified-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=44\n"
            "Enabled=0\n"
            "fix_frame_counted_activity_timer=1\n"
            "fix_frame_counted_smoothing=1\n"
            "fix_id3_tag_divide=1\n"
            "fix_pathfinding_divide=1\n"
            "fix_compressed_mesh_divide=1\n"
            "fix_percent_divide=1\n"
            "fix_element_divide=1\n"
            "fix_object_count_divide=1\n"
            "fix_element_scale_divide=1\n"
            "fix_table_walk_divide=1\n"
            "fix_element_lookup_divide=1\n"
            "fix_count_index_divide=1\n"
            "fix_slot_count_divide=1\n"
            "fix_bucket_size_divide=1\n"
            "fix_hash_mix_divide=1\n"
            "hook_dispatch_probe=1\n"
            "[Graphics]\n"
            "FixFrameRateTimers=1\n"
            "FixFrameRateSmoothing=1\n"
            "[Stability]\n"
            "FixId3TagDivide=1\n"
            "FixPathfindingDivide=1\n"
            "FixCompressedMeshDivide=1\n"
            "FixPercentDivide=1\n"
            "FixElementDivide=1\n"
            "FixObjectCountDivide=1\n"
            "FixElementScaleDivide=1\n"
            "FixTableWalkDivide=1\n"
            "FixElementLookupDivide=1\n"
            "FixCountIndexDivide=1\n"
            "FixSlotCountDivide=1\n"
            "FixBucketSizeDivide=1\n"
            "FixHashMixDivide=1\n"
            "[Advanced]\n"
            "hook_dispatch_probe=1\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        ConfigLoadReport reload_report{};
        const Config reloaded = LoadConfig(path, &reload_report);
        std::ifstream reloaded_stream(path, std::ios::binary);
        const std::string reloaded_text(
            (std::istreambuf_iterator<char>(reloaded_stream)), {});
        constexpr std::array<const char*, 31> retired_names{
            "FixFrameRateTimers=",
            "FixFrameRateSmoothing=",
            "FixId3TagDivide=",
            "FixPathfindingDivide=",
            "FixCompressedMeshDivide=",
            "FixPercentDivide=",
            "FixElementDivide=",
            "FixObjectCountDivide=",
            "FixElementScaleDivide=",
            "FixTableWalkDivide=",
            "FixElementLookupDivide=",
            "FixCountIndexDivide=",
            "FixSlotCountDivide=",
            "FixBucketSizeDivide=",
            "FixHashMixDivide=",
            "fix_frame_counted_activity_timer=",
            "fix_frame_counted_smoothing=",
            "fix_id3_tag_divide=",
            "fix_pathfinding_divide=",
            "fix_compressed_mesh_divide=",
            "fix_percent_divide=",
            "fix_element_divide=",
            "fix_object_count_divide=",
            "fix_element_scale_divide=",
            "fix_table_walk_divide=",
            "fix_element_lookup_divide=",
            "fix_count_index_divide=",
            "fix_slot_count_divide=",
            "fix_bucket_size_divide=",
            "fix_hash_mix_divide=",
            "hook_dispatch_probe=",
        };
        bool retired_names_removed = true;
        for (const char* retired_name : retired_names) {
            retired_names_removed &=
                migrated_text.find(retired_name) == std::string::npos;
        }
        if (!Expect(
                !migrated.enabled && !reloaded.enabled &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.source_version == kConfigVersion &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    retired_names_removed &&
                    backup_text == source_text &&
                    reloaded_text == migrated_text,
                "current v44 retired unverified settings should be ignored "
                "and removed by one backed-up migration")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v38-motion-blur-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=38\n"
            "override_motion_blur=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        ConfigLoadReport reload_report{};
        const Config reloaded = LoadConfig(path, &reload_report);
        if (!Expect(
                migrated.override_motion_blur == 0 &&
                    reloaded.override_motion_blur == 0 &&
                    report.source_version == 38 &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.source_version == kConfigVersion &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("\nMotionBlur=0\n") !=
                        std::string::npos &&
                    migrated_text.find("override_motion_blur=") ==
                        std::string::npos &&
                    backup_text == source_text,
                "v38 MotionBlur should migrate once to the canonical v44 "
                "graphics setting with an exact backup")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    for (const int value : std::array{-1, 0, 1, 2}) {
        const auto path = GetTestTempRoot() /
                          (L"spatch-v42-motion-blur-parse-" +
                           std::to_wstring(value) + L".ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\nConfigVersion=44\n"
            "[Graphics]\nMotionBlur=" +
            std::to_string(value) + "\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }
        ConfigLoadReport report{};
        const Config parsed = LoadConfig(path, &report);
        std::ifstream parsed_stream(path, std::ios::binary);
        const std::string parsed_text(
            (std::istreambuf_iterator<char>(parsed_stream)), {});
        if (!Expect(
                parsed.override_motion_blur == value &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    parsed_text == source_text &&
                    !std::filesystem::exists(backup),
                "current MotionBlur values should parse without rewriting")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }


    for (const int value : std::array{0, 1}) {
        const auto path = GetTestTempRoot() /
                          (L"spatch-v44-debug-crash-dump-parse-" +
                           std::to_wstring(value) + L".ini");
        RemoveIfExists(path);
        const std::string source_text =
            "[SPatch]\nConfigVersion=44\n[Debug]\nWriteCrashDumps=" +
            std::to_string(value) + "\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config parsed = LoadConfig(path, &report);
        std::ifstream parsed_stream(path, std::ios::binary);
        const std::string parsed_text(
            (std::istreambuf_iterator<char>(parsed_stream)), {});
        const Config reloaded = LoadConfig(path);
        std::ifstream reloaded_stream(path, std::ios::binary);
        const std::string reloaded_text(
            (std::istreambuf_iterator<char>(reloaded_stream)), {});
        if (!Expect(
                parsed.write_minidumps == (value != 0) &&
                    reloaded.write_minidumps == (value != 0) &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Unchanged &&
                    parsed_text == source_text && reloaded_text == source_text,
                "current WriteCrashDumps 0/1 values should parse without "
                "rewriting and reload byte-identically")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v42-to-v44-schema-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\r\n"
            "ConfigVersion=42\r\n"
            "Enabled=0\r\n"
            "[Input]\r\n"
            "ForceRawMouseInput=0\r\n"
            "[Debug]\r\n"
            "Logging=0\r\n"
            "WriteCrashDumps=1\r\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        ConfigLoadReport reload_report{};
        const Config reloaded = LoadConfig(path, &reload_report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        if (!Expect(
                !migrated.enabled && !reloaded.enabled &&
                    migrated.write_minidumps && reloaded.write_minidumps &&
                    !migrated.force_raw_mouse_input &&
                    !reloaded.force_raw_mouse_input &&
                    !migrated.gta_iv_car_camera &&
                    !reloaded.gta_iv_car_camera &&
                    !migrated.gta_iv_bike_camera &&
                    !reloaded.gta_iv_bike_camera &&
                    migrated.anisotropic_filtering == 16 &&
                    migrated.force_anisotropic_filtering &&
                    report.source_version == 42 &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.source_version == kConfigVersion &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    backup_text == source_text &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("GTAIVCarCamera=0") !=
                        std::string::npos &&
                    migrated_text.find("GTAIVBikeCamera=0") !=
                        std::string::npos &&
                    migrated_text.ends_with(
                        "; Write a small diagnostic dump if the game crashes.\n"
                        "WriteCrashDumps=1") &&
                    !HasInGameConfigWorkResidue(path),
                "v42 should migrate once to v44, retain public choices, add "
                "both disabled GTA IV vehicle-camera defaults, preserve the "
                "literal crash-dump EOF, and keep an exact external backup")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v42-crash-dump-precedence-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=44\n"
            "WriteCrashDumps=0\n"
            "write_minidumps=0\n"
            "[Debug]\n"
            "WriteCrashDumps=1\n"
            "write_minidumps=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                migrated.write_minidumps &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    migrated_text.ends_with(
                        "; Write a small diagnostic dump if the game crashes.\n"
                        "WriteCrashDumps=1") &&
                    migrated_text.find("write_minidumps=") ==
                        std::string::npos &&
                    backup_text == source_text &&
                    !std::filesystem::exists(
                        InGameLegacyConfigBackupPath(path)),
                "canonical Debug WriteCrashDumps should win over legacy "
                "locations and migrate to the literal final INI key")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    for (const int value : std::array{0, 1}) {
        const auto path = GetTestTempRoot() /
                          (L"spatch-v28-crash-dump-migration-" +
                           std::to_wstring(value) + L".ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string legacy_text =
            "[SPatch]\nConfigVersion=28\nwrite_minidumps=" +
            std::to_string(value) + "\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << legacy_text;
        }

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        const Config reloaded = LoadConfig(path);
        std::ifstream reloaded_stream(path, std::ios::binary);
        const std::string reloaded_text(
            (std::istreambuf_iterator<char>(reloaded_stream)), {});
        if (!Expect(
                migrated.write_minidumps == (value != 0) &&
                    reloaded.write_minidumps == (value != 0) &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("WriteCrashDumps=" +
                                       std::to_string(value)) !=
                        std::string::npos &&
                    migrated_text.find("write_minidumps=") ==
                        std::string::npos &&
                    backup_text == legacy_text &&
                    reloaded_text == migrated_text &&
                    !std::filesystem::exists(
                        InGameLegacyConfigBackupPath(path)) &&
                    migrated_text.ends_with(
                        "; Write a small diagnostic dump if the game crashes.\n"
                        "WriteCrashDumps=" + std::to_string(value)),
                "WriteCrashDumps 0/1 migration should preserve the selection, "
                "back up the source externally, and reload byte-identically")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-final-release-policy-test.ini");
        RemoveIfExists(path);
        const std::string source_text =
            "[SPatch]\n"
             "ConfigVersion=44\n"
            "[Advanced]\n"
            "hook_task_dispatch=1\n"
            "[AntiAliasing]\n"
            "smaa_debug_keys=1\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }
        const Config config = LoadConfig(path);
        std::ifstream reloaded_stream(path, std::ios::binary);
        const std::string reloaded_text(
            (std::istreambuf_iterator<char>(reloaded_stream)), {});
#if defined(SPATCH_FINAL_RELEASE)
        const bool developer_controls_match_policy =
            !config.hook_task_dispatch && !config.smaa_debug_keys;
        constexpr const char* policy_message =
            "SPATCH_FINAL_RELEASE should force task-dispatch and SMAA debug "
            "controls off";
#else
        const bool developer_controls_match_policy =
            config.hook_task_dispatch && config.smaa_debug_keys;
        constexpr const char* policy_message =
            "normal tests should prove current developer controls remain "
            "readable";
#endif
        if (!Expect(developer_controls_match_policy &&
                        reloaded_text == source_text,
                    policy_message)) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto missing_parent =
            GetTestTempRoot() / L"missing-config-parent";
        const auto path = missing_parent / L"SPatch.ini";
        std::error_code error;
        std::filesystem::remove_all(missing_parent, error);
        ConfigLoadReport report{};
        const Config config = LoadConfig(path, &report);
        if (!Expect(
                config.enabled &&
                    report.persistence == ConfigPersistenceStatus::CreateFailed &&
                    !report.persistence_succeeded() &&
                    !std::filesystem::exists(path) &&
                    !WriteBoolValue(path, L"enabled", false),
                "config creation and helper writes must report a missing "
                "parent without creating a one-key INI")) {
            return EXIT_FAILURE;
        }
    }

    {
        const auto game_root = GetTestTempRoot() / L"legacy-backup-game-root";
        std::error_code error;
        std::filesystem::create_directories(game_root, error);
        const auto path = game_root / L"SPatch.ini";
        const auto backup = ConfigBackupPath(path);
        const auto legacy_in_game = InGameLegacyConfigBackupPath(path);
        const auto relocated_legacy = LegacyConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        RemoveIfExists(legacy_in_game);
        RemoveIfExists(relocated_legacy);
        const std::string source_text =
            "[SPatch]\r\nConfigVersion=44\r\nEnabled=0\r\n";
        constexpr char legacy_backup_bytes[] =
            "legacy in-game backup bytes\r\n\0tail";
        const std::string legacy_backup_text(
            legacy_backup_bytes, sizeof(legacy_backup_bytes) - 1);
        {
            std::ofstream source(path, std::ios::binary | std::ios::trunc);
            source << source_text;
            std::ofstream legacy(
                legacy_in_game, std::ios::binary | std::ios::trunc);
            legacy.write(legacy_backup_text.data(),
                         static_cast<std::streamsize>(legacy_backup_text.size()));
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        std::ifstream relocated_stream(relocated_legacy, std::ios::binary);
        const std::string relocated_text(
            (std::istreambuf_iterator<char>(relocated_stream)), {});
        ConfigLoadReport reload_report{};
        const Config reloaded = LoadConfig(path, &reload_report);
        const auto isolated_local_app_data =
            (GetTestTempRoot() / L"LocalAppData").lexically_normal();
        const auto backup_relative =
            backup.lexically_normal().lexically_relative(isolated_local_app_data);
        const auto relocated_relative = relocated_legacy.lexically_normal()
                                            .lexically_relative(
                                                isolated_local_app_data);
        if (!Expect(
                !migrated.enabled && !reloaded.enabled &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    ConfigBackupPath(path) == backup &&
                    backup.filename() == L"SPatch-pre-v44.ini" &&
                    relocated_legacy.filename() ==
                        L"SPatch-legacy-previous.ini" &&
                    !backup_relative.empty() &&
                    *backup_relative.begin() != L".." &&
                    !relocated_relative.empty() &&
                    *relocated_relative.begin() != L".." &&
                    backup.parent_path() != path.parent_path() &&
                    backup_text == source_text &&
                    relocated_text == legacy_backup_text &&
                    !std::filesystem::exists(legacy_in_game) &&
                    !HasInGameConfigWorkResidue(path),
                "a current v44 config should relocate a legacy in-game backup "
                "byte-identically under isolated LocalAppData, preserve the "
                "current source externally, and leave no game-root work file")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
        RemoveIfExists(relocated_legacy);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-config-backup-failure-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        std::error_code error;
        std::filesystem::remove_all(backup, error);
        const std::string legacy =
            "[SPatch]\nconfig_version=1\nenabled=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << legacy;
        }
        std::filesystem::create_directories(backup.parent_path());
        std::filesystem::create_directory(backup);
        ConfigLoadReport report{};
        const Config config = LoadConfig(path, &report);
        std::ifstream stream(path, std::ios::binary);
        const std::string retained((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
        if (!Expect(
                !config.enabled &&
                    report.persistence == ConfigPersistenceStatus::BackupFailed &&
                    retained == legacy,
                "a failed migration backup must preserve the source INI "
                "byte-for-byte and report the failed stage")) {
            return EXIT_FAILURE;
        }
        std::filesystem::remove_all(backup, error);
        RemoveIfExists(path);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-config-publish-failure-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string legacy =
            "[SPatch]\nconfig_version=1\nenabled=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << legacy;
        }
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
        ConfigLoadReport report{};
        const Config config = LoadConfig(path, &report);
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::ifstream stream(path, std::ios::binary);
        const std::string retained((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
        if (!Expect(
                !config.enabled && report.persistence ==
                                       ConfigPersistenceStatus::MigrationWriteFailed &&
                    retained == legacy && std::filesystem::exists(backup),
                "a failed migration publish must retain the old INI and the "
                "durable backup while reporting the failed stage")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }








    {
        const auto path = MakeTempIniPath(L"spatch-v23-contact-fix-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=23\n"
               << "[Graphics]\n"
               << "FixVRAMReporting=0\n"
               << "[Stability]\n"
               << "FixResourceLoading=0\n"
               << "[Diagnostics]\n"
               << "EnableLogging=1\n";
        stream.close();

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        if (!Expect(
                !migrated.fix_vram_capacity_reporting &&
                    !migrated.fix_resource_loading && migrated.enable_logging &&
                    migrated.fix_first_run_resolution &&
                    migrated.fix_scaleform_qpc_clock &&
                    migrated.fix_contact_list_overflow &&
                    migrated_text.find("ConfigVersion=44") != std::string::npos &&
                    migrated_text.find("FixVRAMReporting=0") != std::string::npos &&
                    migrated_text.find("FixResourceLoading=0") != std::string::npos &&
                    migrated_text.find("FixContactListOverflow=1") !=
                        std::string::npos &&
                    migrated_text.find("[Debug]") != std::string::npos &&
                    migrated_text.find("Logging=1") != std::string::npos &&
                    migrated_text.find("EnableLogging=") == std::string::npos &&
                    migrated_text.find("[Diagnostics]") == std::string::npos,
                "v23 migration should preserve public choices and expose the "
                "contact-list fix and canonical logging key")) {
            return EXIT_FAILURE;
        }
        if (!Expect(std::filesystem::exists(backup),
                    "v23 migration should preserve the replaced file")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path = MakeTempIniPath(L"spatch-v21-pcss-removal-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=21\n"
               << "[Graphics]\n"
               << "RestoreCharacterShadows=0\n"
               << "ShadowFilter=0\n"
               << "[Shadows]\n"
               << "ShadowFiltering=PCSS\n"
               << "PCSSQuality=4\n"
               << "PCSSLightSize=250\n";
        stream.close();

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        if (!Expect(
                migrated.override_shadow_filter == 0 &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("[Shadows]") != std::string::npos &&
                    migrated_text.find("OriginalShadowFilter=0") !=
                        std::string::npos &&
                    migrated_text.find("RestoreCharacterShadows=") ==
                        std::string::npos &&
                    migrated_text.find("ShadowFiltering=") ==
                        std::string::npos &&
                    migrated_text.find("PCSSQuality=") == std::string::npos &&
                    migrated_text.find("PCSSLightSize=") == std::string::npos &&
                    migrated_text.find("\nShadowFilter=") == std::string::npos,
                "v21 migration should remove the experimental PCSS settings")) {
            return EXIT_FAILURE;
        }
        if (!Expect(std::filesystem::exists(backup),
                    "v21 shadow migration should preserve the replaced file")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v42-retired-pcss-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=44\n"
            "Enabled=0\n"
            "ShadowFiltering=PCSS\n"
            "shadow_filtering=PCSS\n"
            "PCSS=1\n"
            "PCSSQuality=4\n"
            "pcss_quality=3\n"
            "PCSSLightSize=250\n"
            "pcss_light_size_percent=300\n"
            "[Shadows]\n"
            "ShadowFiltering=PCSS\n"
            "shadow_filtering=PCSS\n"
            "PCSS=1\n"
            "PCSSQuality=4\n"
            "pcss_quality=3\n"
            "PCSSLightSize=250\n"
            "pcss_light_size_percent=300\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        migrated_stream.close();
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        backup_stream.close();
        ConfigLoadReport reload_report{};
        (void)LoadConfig(path, &reload_report);
        if (!Expect(
                !migrated.enabled &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.source_version == kConfigVersion &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    migrated_text.find("ShadowFiltering=") ==
                        std::string::npos &&
                    migrated_text.find("shadow_filtering=") ==
                        std::string::npos &&
                    migrated_text.find("\nPCSS=") == std::string::npos &&
                    migrated_text.find("PCSSQuality=") ==
                        std::string::npos &&
                    migrated_text.find("pcss_quality=") ==
                        std::string::npos &&
                    migrated_text.find("PCSSLightSize=") ==
                        std::string::npos &&
                    migrated_text.find("pcss_light_size_percent=") ==
                        std::string::npos &&
                    backup_text == source_text,
                "current v44 retired PCSS forms should migrate once with an "
                "exact backup")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }





















    {
        const auto path =
            MakeTempIniPath(L"spatch-v19-input-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=19\n"
               << "[Input]\n"
               << "ForceRawMouseInput=0\n"
               << "DisableCameraSmoothing=0\n"
               << "LeftStickDeadzone=-20\n"
               << "RightStickDeadzone=999\n";
        stream.close();

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        if (!Expect(!migrated.force_raw_mouse_input &&
                        !migrated.disable_camera_smoothing &&
                        migrated.controller_left_stick_deadzone == -1 &&
                        migrated.controller_right_stick_deadzone == 95 &&
                        migrated_text.find("ConfigVersion=44") !=
                            std::string::npos &&
                        migrated_text.find("[Input]") != std::string::npos &&
                        migrated_text.find("ForceRawMouseInput=0") !=
                            std::string::npos &&
                        migrated_text.find("DisableCameraSmoothing=0") !=
                            std::string::npos &&
                        migrated_text.find("LeftStickDeadzone=-1") !=
                            std::string::npos &&
                        migrated_text.find("RightStickDeadzone=95") !=
                            std::string::npos &&
                        std::filesystem::exists(backup),
                    "v19 migration should preserve input switches and clamp "
                    "deadzones safely")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        using namespace spatch::input;
        if (!Expect(
                ClampDeadzonePercent(-50) == -1 &&
                    ClampDeadzonePercent(0) == 0 &&
                    ClampDeadzonePercent(37) == 37 &&
                    ClampDeadzonePercent(200) == 95 &&
                    std::fabs(DeadzoneFraction(25) - 0.25f) < 0.0001f &&
                    std::fabs(DeadzoneScale(0.25f) - (4.0f / 3.0f)) < 0.0001f,
                "input policy should preserve stock mode and build stable "
                "radial curves")) {
            return EXIT_FAILURE;
        }
    }


    {
        using spatch::smaa::ShouldSuppressStockAa;
        if (!Expect(!ShouldSuppressStockAa(false, true, true, true, true) &&
                        !ShouldSuppressStockAa(true, false, true, true, true) &&
                        !ShouldSuppressStockAa(true, true, false, true, true) &&
                        !ShouldSuppressStockAa(true, true, true, false, true) &&
                        !ShouldSuppressStockAa(true, true, true, true, false) &&
                        ShouldSuppressStockAa(true, true, true, true, true),
                    "stock AA should fail open until a complete SMAA pass is "
                    "proven")) {
            return EXIT_FAILURE;
        }
        if (!Expect(spatch::smaa::ShouldRunSmaaPresentPass(0) &&
                        !spatch::smaa::ShouldRunSmaaPresentPass(
                            DXGI_PRESENT_TEST) &&
                        spatch::smaa::ShouldRunSmaaPresentPass(
                            DXGI_PRESENT_DO_NOT_WAIT) &&
                        !spatch::smaa::ShouldRunSmaaPresentPass(
                            DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT),
                    "SMAA should skip probes but process every real present")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::smaa::ShouldRestoreSmaaSource(
                    DXGI_PRESENT_DO_NOT_WAIT, DXGI_ERROR_WAS_STILL_DRAWING) &&
                    spatch::smaa::ShouldRestoreSmaaSource(0, E_FAIL) &&
                    spatch::smaa::ShouldRestoreSmaaSource(
                        DXGI_PRESENT_DO_NOT_WAIT, E_FAIL) &&
                    !spatch::smaa::ShouldRestoreSmaaSource(
                        DXGI_PRESENT_DO_NOT_WAIT, S_OK) &&
                    !spatch::smaa::ShouldRestoreSmaaSource(
                        DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT,
                        DXGI_ERROR_WAS_STILL_DRAWING),
                "SMAA should restore source after every failed real present")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!spatch::smaa::ShouldPublishSmaaSuccess(false, S_OK) &&
                        spatch::smaa::ShouldPublishSmaaSuccess(true, S_OK) &&
                        spatch::smaa::ShouldPublishSmaaSuccess(
                            true, DXGI_STATUS_OCCLUDED) &&
                        !spatch::smaa::ShouldPublishSmaaSuccess(true, E_FAIL) &&
                        !spatch::smaa::ShouldPublishSmaaSuccess(
                            true, DXGI_ERROR_WAS_STILL_DRAWING),
                    "SMAA success should require a complete pass and a "
                    "successful Present")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !spatch::smaa::ShouldInvalidateAfterSkippedPresent(
                    DXGI_PRESENT_TEST, S_OK) &&
                    spatch::smaa::ShouldInvalidateAfterSkippedPresent(
                        DXGI_PRESENT_TEST, E_FAIL) &&
                    spatch::smaa::ShouldInvalidateAfterSkippedPresent(
                        DXGI_PRESENT_DO_NOT_WAIT, S_OK) &&
                    spatch::smaa::ShouldInvalidateAfterSkippedPresent(0, S_OK),
                "SMAA should preserve successful test probes but fail open "
                "after failed probes and skipped real presents")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::smaa::SmaaTypelessFormat(DXGI_FORMAT_R8G8B8A8_UNORM) ==
                        DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                    spatch::smaa::SmaaLinearFormat(
                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
                        DXGI_FORMAT_R8G8B8A8_UNORM &&
                    spatch::smaa::SmaaSrgbFormat(
                        DXGI_FORMAT_R8G8B8A8_TYPELESS) ==
                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
                    spatch::smaa::SmaaTypelessFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM) ==
                        DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                    spatch::smaa::SmaaLinearFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ==
                        DXGI_FORMAT_B8G8R8A8_UNORM &&
                    spatch::smaa::SmaaSrgbFormat(
                        DXGI_FORMAT_B8G8R8A8_TYPELESS) ==
                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
                    spatch::smaa::SmaaTypelessFormat(
                        DXGI_FORMAT_R10G10B10A2_UNORM) == DXGI_FORMAT_UNKNOWN,
                "SMAA should expose canonical linear and sRGB views only for "
                "supported SDR backbuffers")) {
            return EXIT_FAILURE;
        }
    }

    {
        using spatch::pedestrian_timing::FixedRateScheduler;

        for (const int fps : {30, 60, 120, 144, 240}) {
            FixedRateScheduler scheduler;
            std::uint32_t calls = 0;
            for (int frame = 0; frame < fps * 10; ++frame) {
                calls +=
                    scheduler.Advance(1.0 / static_cast<double>(fps)).steps;
            }
            if (!Expect(
                    calls >= 299 && calls <= 300,
                    "pedestrian throttle should remain at 30 Hz across render "
                    "rates")) {
                return EXIT_FAILURE;
            }
        }

        FixedRateScheduler hitch_scheduler;
        const auto hitch = hitch_scheduler.Advance(1.0);
        if (!Expect(
                hitch.frame_delta_clamped &&
                    hitch.steps ==
                        spatch::pedestrian_timing::kMaximumStepsPerFrame &&
                    hitch_scheduler.remainder_seconds() <
                        spatch::pedestrian_timing::kStockThrottleStepSeconds,
                "pedestrian throttle should bound catch-up after a long "
                "frame")) {
            return EXIT_FAILURE;
        }
        FixedRateScheduler short_hitch_scheduler;
        if (!Expect(
                short_hitch_scheduler.Advance(0.0).steps == 0 &&
                    short_hitch_scheduler.Advance(0.1).steps == 1,
                "paused and hitched frames should run at most one stock crowd "
                "update")) {
            return EXIT_FAILURE;
        }
        const auto invalid = hitch_scheduler.Advance(-1.0);
        if (!Expect(invalid.steps == 0 &&
                        hitch_scheduler.remainder_seconds() == 0.0,
                    "invalid pedestrian deltas should reset the scheduler")) {
            return EXIT_FAILURE;
        }
    }

    {
        const auto chase = average_window::ResolveCapacity(2.0f, 30.0f);
        const auto follow = average_window::ResolveCapacity(1.0f, 60.0f);
        const auto unknown = average_window::ResolveCapacity(2.0f, 120.0f);
        const auto oversized = average_window::ResolveCapacity(5.0f, 30.0f);
        if (!Expect(chase.expanded && chase.effective_sample_rate == 1000.0f &&
                        chase.entry_count == 2002 && follow.expanded &&
                        follow.entry_count == 1002,
                    "high-FPS average windows should cover their full timespan "
                    "through 1000 FPS")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!unknown.expanded && !unknown.recognized_stock_rate &&
                        !oversized.expanded && oversized.recognized_stock_rate,
                    "unknown or oversized average windows should retain stock "
                    "allocation")) {
            return EXIT_FAILURE;
        }
        for (const int fps : {30, 60, 120, 144, 240, 360, 500, 1000}) {
            const std::size_t samples_needed =
                static_cast<std::size_t>(2 * fps) + 2;
            if (!Expect(
                    chase.entry_count >= samples_needed,
                    "expanded chase-camera history should not evict early at "
                    "supported FPS")) {
                return EXIT_FAILURE;
            }
        }
    }

    {
        const auto automatic =
            spatch::engine_fixes::ResolveReflectionResolution(0, 3840);
        const auto oversized =
            spatch::engine_fixes::ResolveReflectionResolution(9999, 1920);
        const auto odd =
            spatch::engine_fixes::ResolveReflectionResolution(1921, 1920);
        if (!Expect(
                automatic.width == 3840 && automatic.height == 1920 &&
                    oversized.width == 4096 && oversized.height == 2048 &&
                    odd.width == 1920 && odd.height == 960,
                "spherical reflections should stay bounded and exactly 2:1")) {
            return EXIT_FAILURE;
        }

        const std::uintptr_t safe_pointer = 0x10000;
        if (!Expect(
                !spatch::engine_fixes::IsSafeSavePayload(0, 0x84) &&
                    !spatch::engine_fixes::IsSafeSavePayload(safe_pointer,
                                                             0x83) &&
                    spatch::engine_fixes::IsSafeSavePayload(safe_pointer,
                                                            0x84) &&
                    spatch::engine_fixes::IsSafeSavePayload(
                        safe_pointer, static_cast<std::uint32_t>(
                                          (std::numeric_limits<int>::max)())) &&
                    !spatch::engine_fixes::IsSafeSavePayload(safe_pointer,
                                                             0x80000000u) &&
                    !spatch::engine_fixes::IsSafeSavePayload(
                        (std::numeric_limits<std::uintptr_t>::max)() - 0x82,
                        0x84),
                "save parser guard should enforce table, signed-size, null, "
                "and "
                "overflow bounds")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !spatch::engine_fixes::IsSafeSaveFile(safe_pointer, 0xB7) &&
                    spatch::engine_fixes::IsSafeSaveFile(safe_pointer, 0xB8),
                "local-save header guard should reject files shorter than "
                "header plus payload")) {
            return EXIT_FAILURE;
        }
        if (!Expect(spatch::engine_fixes::NormalizeThreadHandle(0) ==
                            (std::numeric_limits<std::uintptr_t>::max)() &&
                        spatch::engine_fixes::NormalizeThreadHandle(0x1234) ==
                            0x1234,
                    "thread failure adapter should translate only NULL to the "
                    "engine sentinel")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::engine_fixes::AreTaskManagerEventsReady(1, 2, 3, 4) &&
                    !spatch::engine_fixes::AreTaskManagerEventsReady(0, 2, 3,
                                                                     4) &&
                    !spatch::engine_fixes::AreTaskManagerEventsReady(1, 0, 3,
                                                                     4) &&
                    !spatch::engine_fixes::AreTaskManagerEventsReady(1, 2, 0,
                                                                     4) &&
                    !spatch::engine_fixes::AreTaskManagerEventsReady(1, 2, 3,
                                                                     0),
                "task-manager bootstrap should require every synchronization "
                "event")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::engine_fixes::AreIoThreadBootstrapEventsReady(1, 2, 3,
                                                                      4) &&
                    !spatch::engine_fixes::AreIoThreadBootstrapEventsReady(
                        0, 2, 3, 4) &&
                    !spatch::engine_fixes::AreIoThreadBootstrapEventsReady(
                        1, 0, 3, 4) &&
                    !spatch::engine_fixes::AreIoThreadBootstrapEventsReady(
                        1, 2, 0, 4) &&
                    !spatch::engine_fixes::AreIoThreadBootstrapEventsReady(
                        1, 2, 3, 0),
                "I/O thread bootstrap should require every event in the worker "
                "wait set")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::engine_fixes::AreBankManagerEventsReady(1, 2) &&
                    !spatch::engine_fixes::AreBankManagerEventsReady(0, 2) &&
                    !spatch::engine_fixes::AreBankManagerEventsReady(1, 0),
                "BankManager bootstrap should require its wake and "
                "callback-fence events")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::engine_fixes::SelectCreatedOrReservedEvent(
                    0x10, 0x20) == 0x10 &&
                    spatch::engine_fixes::SelectCreatedOrReservedEvent(
                        0, 0x20) == 0x20 &&
                    spatch::engine_fixes::SelectCreatedOrReservedEvent(0, 0) ==
                        0,
                "event allocation fallback should prefer a created handle and "
                "otherwise "
                "transfer only the reserve")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !spatch::engine_fixes::IsWwiseBlockingOperationPending(0) &&
                    spatch::engine_fixes::IsWwiseBlockingOperationPending(1) &&
                    spatch::engine_fixes::IsWwiseBlockingOperationPending(0xFF),
                "Wwise blocking I/O fallback should wait for every nonzero "
                "pending state")) {
            return EXIT_FAILURE;
        }
        const auto forward_rel =
            spatch::engine_fixes::ComputeRelativeBranchDisplacement(0x1000, 5,
                                                                    0x2000);
        const auto backward_rel =
            spatch::engine_fixes::ComputeRelativeBranchDisplacement(0x2000, 5,
                                                                    0x1000);
        const auto far_rel =
            spatch::engine_fixes::ComputeRelativeBranchDisplacement(
                0x1000, 5,
                0x1000ull +
                    static_cast<std::uintptr_t>(
                        (std::numeric_limits<int>::max)()) +
                    6ull);
        if (!Expect(
                forward_rel == 0xFFB && backward_rel == -0x1005 &&
                    !far_rel.has_value(),
                "near relays should encode only exact signed rel32 branches")) {
            return EXIT_FAILURE;
        }

        constexpr std::array<std::uint8_t, 3> stock{1, 2, 3};
        constexpr std::array<std::uint8_t, 3> replacement{4, 5, 6};
        constexpr std::array<std::uint8_t, 3> unknown{7, 8, 9};
        if (!Expect(spatch::runtime_patch::ClassifyBytes(stock, stock,
                                                         replacement) ==
                            spatch::runtime_patch::ByteState::Expected &&
                        spatch::runtime_patch::ClassifyBytes(replacement, stock,
                                                             replacement) ==
                            spatch::runtime_patch::ByteState::Replacement &&
                        spatch::runtime_patch::ClassifyBytes(unknown, stock,
                                                             replacement) ==
                            spatch::runtime_patch::ByteState::Unexpected,
                    "runtime patches should distinguish stock, compatible, and "
                    "unknown "
                    "bytes")) {
            return EXIT_FAILURE;
        }
        if (!Expect(spatch::runtime_patch::IsAddressInPatchRange(0x1000, 0x1000,
                                                                 6) &&
                        spatch::runtime_patch::IsAddressInPatchRange(
                            0x1005, 0x1000, 6) &&
                        !spatch::runtime_patch::IsAddressInPatchRange(
                            0x0FFF, 0x1000, 6) &&
                        !spatch::runtime_patch::IsAddressInPatchRange(
                            0x1006, 0x1000, 6) &&
                        !spatch::runtime_patch::IsAddressInPatchRange(
                            0x1000, 0x1000, 0),
                    "runtime patches should reject suspended RIPs in the exact "
                    "half-open write range")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!spatch::runtime_patch::ThreadSnapshotPassNeedsRetry(
                        false, false) &&
                        spatch::runtime_patch::ThreadSnapshotPassNeedsRetry(
                            true, false) &&
                        spatch::runtime_patch::ThreadSnapshotPassNeedsRetry(
                            false, true) &&
                        spatch::runtime_patch::ThreadSnapshotPassNeedsRetry(
                            true, true),
                    "thread freeze should retry both newly-suspended and "
                    "exit-raced "
                    "snapshots")) {
            return EXIT_FAILURE;
        }

        void* patch_page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
        if (!Expect(patch_page != nullptr,
                    "runtime patch test page should allocate")) {
            return EXIT_FAILURE;
        }
        std::memcpy(patch_page, stock.data(), stock.size());
        DWORD old_protection = 0;
        const bool made_executable =
            VirtualProtect(patch_page, 4096, PAGE_EXECUTE_READ,
                           &old_protection) != FALSE;
        spatch::runtime_patch::Registry registry;
        const auto apply_result = registry.Apply(
            "unit_test", reinterpret_cast<std::uintptr_t>(patch_page), stock,
            replacement);
        const bool applied =
            apply_result == spatch::runtime_patch::ApplyResult::Applied &&
            std::memcmp(patch_page, replacement.data(), replacement.size()) ==
                0;
        const std::size_t checkpoint = registry.checkpoint();
        auto* const second_site = static_cast<std::uint8_t*>(patch_page) + 16;
        DWORD writable_protection = 0;
        VirtualProtect(patch_page, 4096, PAGE_READWRITE, &writable_protection);
        std::memcpy(second_site, stock.data(), stock.size());
        DWORD executable_protection = 0;
        VirtualProtect(patch_page, 4096, PAGE_EXECUTE_READ,
                       &executable_protection);
        const auto second_result = registry.Apply(
            "unit_test_second", reinterpret_cast<std::uintptr_t>(second_site),
            stock, replacement);
        const bool restored_to_checkpoint =
            second_result == spatch::runtime_patch::ApplyResult::Applied &&
            registry.RestoreTo(checkpoint) &&
            std::memcmp(patch_page, replacement.data(), replacement.size()) ==
                0 &&
            std::memcmp(second_site, stock.data(), stock.size()) == 0;
        const bool restored =
            restored_to_checkpoint && registry.RestoreAll() &&
            std::memcmp(patch_page, stock.data(), stock.size()) == 0;
        MEMORY_BASIC_INFORMATION restored_region{};
        const bool restored_protection =
            VirtualQuery(patch_page, &restored_region,
                         sizeof(restored_region)) == sizeof(restored_region) &&
            restored_region.Protect == PAGE_EXECUTE_READ;
        VirtualFree(patch_page, 0, MEM_RELEASE);
        if (!Expect(
                made_executable && applied && restored && restored_protection,
                "runtime patch checkpoints should restore bytes and original "
                "page protection")) {
            return EXIT_FAILURE;
        }
    }

    {
        constexpr std::array<std::uint8_t, 9> invalid_resolution{
            0x31, 0x39, 0x32, 0x30, 0x78, 0x31, 0x38, 0x38, 0x30};
        constexpr std::array<std::uint8_t, 9> valid_resolution{
            0x31, 0x39, 0x32, 0x30, 0x78, 0x31, 0x30, 0x38, 0x30};
        constexpr std::array<std::uint8_t, 15> truncated_qpc_conversion{
            0x48, 0x69, 0xDB, 0x40, 0x42, 0x0F, 0x00, 0x33,
            0xD2, 0x48, 0x8B, 0xC3, 0x49, 0xF7, 0xF0};
        constexpr std::array<std::uint8_t, 15> full_qpc_conversion{
            0x48, 0x8B, 0xC3, 0xB9, 0x40, 0x42, 0x0F, 0x00,
            0x48, 0xF7, 0xE1, 0x49, 0xF7, 0xF0, 0x90};
        constexpr std::array<std::uint8_t, 8> timestamp_open_existing{
            0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 8> timestamp_open_always{
            0xC7, 0x44, 0x24, 0x20, 0x04, 0x00, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 3> null_file_handle_test{0x48, 0x85,
                                                                    0xC0};
        constexpr std::array<std::uint8_t, 3> invalid_file_handle_test{
            0x48, 0xFF, 0xC0};
        constexpr std::array<std::uint8_t, 3> mapping_argument_from_rax{
            0x48, 0x8B, 0xC8};
        constexpr std::array<std::uint8_t, 3> mapping_argument_from_rbx{
            0x48, 0x8B, 0xCB};
        constexpr std::array<std::uint8_t, 14> truncated_file_size_combine{
            0x8B, 0x55, 0x08, 0x03, 0x55, 0x0C, 0x49,
            0x8B, 0xCE, 0xE8, 0x77, 0xDB, 0xFF, 0xFF};
        constexpr std::array<std::uint8_t, 14> full_file_size_combine{
            0x48, 0x8B, 0x55, 0x08, 0x48, 0xC1, 0xCA,
            0x20, 0x49, 0x89, 0x56, 0x08, 0x66, 0x90};
        constexpr std::array<std::uint8_t, 9> legacy_truncated_vram_read{
            0x8B, 0x05, 0x8C, 0x85, 0x9F, 0x01, 0xC1, 0xE8, 0x14};
        constexpr std::array<std::uint8_t, 9> latest_truncated_vram_read{
            0x8B, 0x05, 0x0C, 0x86, 0x9F, 0x01, 0xC1, 0xE8, 0x14};
        constexpr std::array<std::uint8_t, 5> legacy_vram_validation_call{
            0xE8, 0x8E, 0x93, 0xFF, 0xFF};
        constexpr std::array<std::uint8_t, 5> latest_vram_validation_call{
            0xE8, 0x6E, 0x93, 0xFF, 0xFF};
        constexpr std::array<std::uint8_t, 5> legacy_wetness_copy_call{
            0xE8, 0x3F, 0x69, 0xA3, 0x00};
        constexpr std::array<std::uint8_t, 5> latest_wetness_copy_call{
            0xE8, 0xAF, 0x67, 0xA3, 0x00};
        constexpr std::array<std::uint8_t, 5> legacy_loaded_chunk_error_call{
            0xE8, 0x63, 0x6C, 0xE8, 0xFF};
        constexpr std::array<std::uint8_t, 5> latest_loaded_chunk_error_call{
            0xE8, 0x03, 0x6D, 0xE8, 0xFF};
        constexpr std::array<std::uint8_t, 5>
            legacy_loaded_chunk_file_error_call{0xE8, 0x39, 0xB2, 0xF4, 0xFF};
        constexpr std::array<std::uint8_t, 5>
            latest_loaded_chunk_file_error_call{0xE8, 0x19, 0xB1, 0xF4, 0xFF};
        constexpr std::array<std::uint8_t, 5> legacy_file_size_cleanup_call{
            0xE8, 0x54, 0xCA, 0xFF, 0xFF};
        constexpr std::array<std::uint8_t, 5> latest_file_size_cleanup_call{
            0xE8, 0x74, 0xCA, 0xFF, 0xFF};
        constexpr std::array<std::uint8_t, 5> legacy_synchronous_finalize_call{
            0xE8, 0x7E, 0xAD, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 5> latest_synchronous_finalize_call{
            0xE8, 0x5E, 0xAD, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 6>
            synchronous_loose_open_failure_branch{0x0F, 0x84, 0x2A,
                                                  0x01, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 5>
            legacy_synchronous_loose_finalize_call{0xE8, 0x31, 0x8A, 0x00,
                                                   0x00};
        constexpr std::array<std::uint8_t, 5>
            latest_synchronous_loose_finalize_call{0xE8, 0x11, 0x8A, 0x00,
                                                   0x00};
        constexpr std::array<std::uint8_t, 10>
            synchronous_loose_invalid_size_state{0xC7, 0x87, 0xB0, 0x00, 0x00,
                                                 0x00, 0x02, 0x00, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 5> legacy_qcmp_failure_copy_call{
            0xE8, 0xD7, 0x09, 0x8B, 0x00};
        constexpr std::array<std::uint8_t, 5> latest_qcmp_failure_copy_call{
            0xE8, 0xA7, 0x08, 0x8B, 0x00};
        constexpr std::array<std::uint8_t, 5>
            legacy_compressed_xml_allocation_call{0xE8, 0xBE, 0xD5, 0x0F,
                                                   0x00};
        constexpr std::array<std::uint8_t, 5>
            latest_compressed_xml_allocation_call{0xE8, 0xCE, 0xD2, 0x0F,
                                                   0x00};
        constexpr std::array<std::uint8_t, 6> compressed_xml_finalize{
            0xC6, 0x04, 0x3B, 0x00, 0xEB, 0x02};
        constexpr std::array<std::uint8_t, 14> raw_mouse_signature{
            0x48, 0x8B, 0x87, 0x10, 0x18, 0x00, 0x00,
            0x0F, 0xB6, 0x88, 0xBE, 0x02, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 7> forced_raw_mouse_read{
            0xB9, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90};
        constexpr std::array<std::uint8_t, 52> controller_response_signature{
            0xFF, 0xFF, 0xF9, 0x3C, 0x5C, 0x8F, 0x42, 0x3E, 0x5C, 0xEC, 0xAC,
            0x3E, 0xF5, 0x6D, 0xBF, 0x3E, 0x6E, 0xDB, 0xB6, 0x3F, 0x1D, 0x47,
            0xC1, 0x3F, 0x80, 0x6F, 0xCC, 0x3F, 0xCA, 0x6B, 0xA8, 0x40, 0x00,
            0x00, 0x00, 0xB8, 0xFF, 0xFF, 0xF9, 0xBC, 0x5C, 0x8F, 0x42, 0xBE,
            0x5C, 0x8F, 0xC2, 0xBE, 0xCA, 0x6B, 0xA8, 0xC0};
        constexpr std::array<std::uint8_t, 48> mouse_camera_signature{
            0x0A, 0xD7, 0x23, 0x3D, 0x0A, 0xD7, 0xA3, 0x3D, 0x0A, 0xD7,
            0x23, 0x3D, 0x0A, 0xD7, 0xA3, 0x3D, 0x00, 0x00, 0x20, 0x41,
            0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x48, 0x42, 0x00, 0x00,
            0x48, 0x42, 0x9A, 0x99, 0x99, 0x3E, 0x8F, 0xC2, 0x75, 0x3D,
            0x00, 0x00, 0x80, 0x40, 0x66, 0x66, 0x66, 0x3F};
        struct CorePatchLayout {
            bool latest_steam = false;
            std::uintptr_t resolution_rva = 0;
            std::uintptr_t qpc_clock_rva = 0;
            std::uintptr_t timestamp_open_mode_rva = 0;
            std::uintptr_t audio_file_handle_test_rva = 0;
            std::uintptr_t audio_file_mapping_argument_rva = 0;
            std::uintptr_t file_size_combine_rva = 0;
            std::uintptr_t benchmark_vram_read_rva = 0;
            std::uintptr_t vram_validation_call_rva = 0;
            std::uintptr_t vram_validation_function_rva = 0;
            std::uintptr_t wetness_copy_call_rva = 0;
            std::uintptr_t loaded_chunk_error_call_rva = 0;
            std::uintptr_t loaded_chunk_file_error_call_rva = 0;
            std::uintptr_t file_size_cleanup_call_rva = 0;
            std::uintptr_t loaded_chunk_error_logger_rva = 0;
            std::uintptr_t loaded_chunk_file_error_logger_rva = 0;
            std::uintptr_t release_resource_waiters_rva = 0;
            std::uintptr_t file_size_epilogue_rva = 0;
            std::uintptr_t synchronous_finalize_call_rva = 0;
            std::uintptr_t synchronous_loose_open_failure_branch_rva = 0;
            std::uintptr_t synchronous_loose_finalize_call_rva = 0;
            std::uintptr_t synchronous_loose_invalid_size_state_rva = 0;
            std::uintptr_t synchronous_loose_open_failure_epilogue_rva = 0;
            std::uintptr_t resource_finalize_rva = 0;
            std::uintptr_t qcmp_failure_copy_call_rva = 0;
            std::uintptr_t buffer_copy_rva = 0;
            std::uintptr_t compressed_xml_allocation_call_rva = 0;
            std::uintptr_t compressed_xml_finalize_rva = 0;
            std::uintptr_t compressed_xml_cleanup_rva = 0;
            std::uintptr_t resource_allocator_rva = 0;
            std::uintptr_t resource_free_rva = 0;
        };
        constexpr std::array layouts{
            CorePatchLayout{false,      0x01788130, 0x0098AC44, 0x00A3938A,
                            0x00A34B7B, 0x00A34B97, 0x0128E04B, 0x00A4161E,
                            0x0016E33D, 0x001676D0, 0x000039CC, 0x0017B388,
                            0x0017B562, 0x0017B797, 0x00001FF0, 0x000C67A0,
                            0x001781F0, 0x0017B7D9, 0x00174EFD, 0x00177134,
                            0x0017724A, 0x00177295, 0x00177264, 0x0017FC80,
                            0x00189934, 0x00A3A310, 0x0008A61D, 0x0008A643,
                            0x0008A64B, 0x00187BE0, 0x0016E720},
            CorePatchLayout{true,       0x01788250, 0x0098AFE4, 0x00A3932A,
                            0x00A34ABB, 0x00A34AD7, 0x0128DCBB, 0x00A4159E,
                            0x0016E38D, 0x00167700, 0x00003ADC, 0x0017B408,
                            0x0017B5E2, 0x0017B817, 0x00002110, 0x000C6700,
                            0x00178290, 0x0017B859, 0x00174F9D, 0x001771D4,
                            0x001772EA, 0x00177335, 0x00177304, 0x0017FD00,
                            0x001899E4, 0x00A3A290, 0x0008A9BD, 0x0008A9E3,
                            0x0008A9EB, 0x00187C90, 0x0016E770},
        };

        Config config{};
        config.improve_spherical_reflections = false;
        config.remove_hidden_120_fps_cap = false;
        config.fix_corrupt_save_handling = false;
        config.fix_thread_creation_failure = false;
        config.fix_contact_list_overflow = false;
        config.controller_left_stick_deadzone = 0;
        config.controller_right_stick_deadzone = 12;

        for (const CorePatchLayout& layout : layouts) {
            constexpr std::size_t image_size = 0x0243B000;
            constexpr std::uintptr_t page_mask = ~std::uintptr_t{0xFFF};
            void* const image =
                VirtualAlloc(nullptr, image_size, MEM_RESERVE, PAGE_NOACCESS);
            if (!Expect(image != nullptr,
                        "core patch test image should reserve")) {
                return EXIT_FAILURE;
            }

            auto* const image_bytes = static_cast<std::uint8_t*>(image);
            const texture_filtering::AddressProfile texture_filtering_addresses =
                texture_filtering::SelectAddresses(layout.latest_steam);
            auto* const sampler_builder_page = static_cast<std::uint8_t*>(
                VirtualAlloc(
                    image_bytes +
                        (texture_filtering_addresses.sampler_builder_rva &
                         page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resolution_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + (layout.resolution_rva & page_mask),
                             0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const qpc_clock_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + (layout.qpc_clock_rva & page_mask),
                             0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const timestamp_open_mode_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.timestamp_open_mode_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const audio_file_open_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + (layout.audio_file_handle_test_rva &
                                            page_mask),
                             0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const file_size_combine_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.file_size_combine_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const benchmark_vram_read_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.benchmark_vram_read_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const adapter_state_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + 0x02439000, 0x1000, MEM_COMMIT,
                             PAGE_READWRITE));
            auto* const adapter_array_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + 0x0243A000, 0x1000, MEM_COMMIT,
                             PAGE_READWRITE));
            auto* const vram_validation_call_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.vram_validation_call_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const vram_validation_function_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.vram_validation_function_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const wetness_copy_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.wetness_copy_call_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_callback_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + (layout.loaded_chunk_error_call_rva &
                                            page_mask),
                             0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_error_logger_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.loaded_chunk_error_logger_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_file_logger_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.loaded_chunk_file_error_logger_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_waiter_cleanup_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.release_resource_waiters_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_work_pending_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + 0x0225A000, 0x1000, MEM_COMMIT,
                             PAGE_READWRITE));
            auto* const resource_allocator_instance_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + 0x02258000, 0x1000, MEM_COMMIT,
                    PAGE_READWRITE));
            auto* const synchronous_finalize_call_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.synchronous_finalize_call_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const synchronous_loose_loader_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.synchronous_loose_open_failure_branch_rva &
                         page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_finalize_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.resource_finalize_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const qcmp_failure_copy_call_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.qcmp_failure_copy_call_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const buffer_copy_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.buffer_copy_rva & page_mask), 0x1000,
                    MEM_COMMIT, PAGE_READWRITE));
            auto* const compressed_xml_loader_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes +
                        (layout.compressed_xml_allocation_call_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const resource_allocator_page =
                static_cast<std::uint8_t*>(VirtualAlloc(
                    image_bytes + (layout.resource_allocator_rva & page_mask),
                    0x1000, MEM_COMMIT, PAGE_READWRITE));
            auto* const raw_mouse_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + 0x02000000, 0x1000, MEM_COMMIT,
                             PAGE_READWRITE));
            auto* const controller_response_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + 0x02001000, 0x1000, MEM_COMMIT,
                             PAGE_READWRITE));
            auto* const mouse_camera_page = static_cast<std::uint8_t*>(
                VirtualAlloc(image_bytes + 0x02002000, 0x1000, MEM_COMMIT,
                             PAGE_READWRITE));
            if (!Expect(sampler_builder_page != nullptr &&
                            resolution_page != nullptr &&
                            qpc_clock_page != nullptr &&
                            timestamp_open_mode_page != nullptr &&
                            audio_file_open_page != nullptr &&
                            file_size_combine_page != nullptr &&
                            benchmark_vram_read_page != nullptr &&
                            adapter_state_page != nullptr &&
                            adapter_array_page != nullptr &&
                            vram_validation_call_page != nullptr &&
                            vram_validation_function_page != nullptr &&
                            wetness_copy_page != nullptr &&
                            resource_callback_page != nullptr &&
                            resource_error_logger_page != nullptr &&
                            resource_file_logger_page != nullptr &&
                            resource_waiter_cleanup_page != nullptr &&
                            resource_work_pending_page != nullptr &&
                            resource_allocator_instance_page != nullptr &&
                            raw_mouse_page != nullptr &&
                            synchronous_finalize_call_page != nullptr &&
                            synchronous_loose_loader_page != nullptr &&
                            resource_finalize_page != nullptr &&
                            qcmp_failure_copy_call_page != nullptr &&
                            buffer_copy_page != nullptr &&
                            compressed_xml_loader_page != nullptr &&
                            resource_allocator_page != nullptr &&
                            controller_response_page != nullptr &&
                            mouse_camera_page != nullptr,
                        "core patch test pages should commit")) {
                VirtualFree(image, 0, MEM_RELEASE);
                return EXIT_FAILURE;
            }

            auto* const sampler_builder_site =
                image_bytes + texture_filtering_addresses.sampler_builder_rva;
            auto* const force_anisotropic_filtering_site =
                image_bytes +
                texture_filtering_addresses.force_trilinear_instruction_rva;
            auto* const resolution_site = image_bytes + layout.resolution_rva;
            auto* const qpc_clock_site = image_bytes + layout.qpc_clock_rva;
            auto* const timestamp_open_mode_site =
                image_bytes + layout.timestamp_open_mode_rva;
            auto* const audio_file_handle_test_site =
                image_bytes + layout.audio_file_handle_test_rva;
            auto* const audio_file_mapping_argument_site =
                image_bytes + layout.audio_file_mapping_argument_rva;
            auto* const file_size_combine_site =
                image_bytes + layout.file_size_combine_rva;
            auto* const benchmark_vram_read_site =
                image_bytes + layout.benchmark_vram_read_rva;
            auto* const vram_validation_call_site =
                image_bytes + layout.vram_validation_call_rva;
            auto* const vram_validation_function =
                image_bytes + layout.vram_validation_function_rva;
            auto* const wetness_copy_call_site =
                image_bytes + layout.wetness_copy_call_rva;
            auto* const loaded_chunk_error_call_site =
                image_bytes + layout.loaded_chunk_error_call_rva;
            auto* const loaded_chunk_file_error_call_site =
                image_bytes + layout.loaded_chunk_file_error_call_rva;
            auto* const file_size_cleanup_call_site =
                image_bytes + layout.file_size_cleanup_call_rva;
            auto* const loaded_chunk_error_logger =
                image_bytes + layout.loaded_chunk_error_logger_rva;
            auto* const loaded_chunk_file_error_logger =
                image_bytes + layout.loaded_chunk_file_error_logger_rva;
            auto* const release_resource_waiters =
                image_bytes + layout.release_resource_waiters_rva;
            auto* const file_size_epilogue =
                image_bytes + layout.file_size_epilogue_rva;
            auto* const resource_work_pending = image_bytes + 0x0225A64B;
            auto** const resource_allocator_instance =
                reinterpret_cast<void**>(image_bytes + 0x02258190);
            auto* const synchronous_finalize_call_site =
                image_bytes + layout.synchronous_finalize_call_rva;
            auto* const synchronous_loose_open_failure_branch_site =
                image_bytes + layout.synchronous_loose_open_failure_branch_rva;
            auto* const synchronous_loose_finalize_call_site =
                image_bytes + layout.synchronous_loose_finalize_call_rva;
            auto* const synchronous_loose_invalid_size_state_site =
                image_bytes + layout.synchronous_loose_invalid_size_state_rva;
            auto* const synchronous_loose_open_failure_epilogue =
                image_bytes +
                layout.synchronous_loose_open_failure_epilogue_rva;
            auto* const resource_finalize =
                image_bytes + layout.resource_finalize_rva;
            auto* const qcmp_failure_copy_call_site =
                image_bytes + layout.qcmp_failure_copy_call_rva;
            auto* const buffer_copy = image_bytes + layout.buffer_copy_rva;
            auto* const compressed_xml_allocation_call_site =
                image_bytes + layout.compressed_xml_allocation_call_rva;
            auto* const compressed_xml_finalize_site =
                image_bytes + layout.compressed_xml_finalize_rva;
            auto* const compressed_xml_cleanup =
                image_bytes + layout.compressed_xml_cleanup_rva;
            auto* const resource_allocator =
                image_bytes + layout.resource_allocator_rva;
            auto* const resource_free = image_bytes + layout.resource_free_rva;
            auto* const raw_mouse_site = raw_mouse_page + 0x80;
            auto* const raw_mouse_state = raw_mouse_page + 0x200;
            auto* const controller_response_site =
                controller_response_page + 0x80;
            auto* const mouse_camera_site = mouse_camera_page + 0x80;
            const auto& stock_vram_validation_call =
                layout.latest_steam ? latest_vram_validation_call
                                    : legacy_vram_validation_call;
            const auto& stock_benchmark_vram_read =
                layout.latest_steam ? latest_truncated_vram_read
                                    : legacy_truncated_vram_read;
            const auto& stock_wetness_copy_call =
                layout.latest_steam ? latest_wetness_copy_call
                                    : legacy_wetness_copy_call;
            const auto& stock_loaded_chunk_error_call =
                layout.latest_steam ? latest_loaded_chunk_error_call
                                    : legacy_loaded_chunk_error_call;
            const auto& stock_loaded_chunk_file_error_call =
                layout.latest_steam ? latest_loaded_chunk_file_error_call
                                    : legacy_loaded_chunk_file_error_call;
            const auto& stock_file_size_cleanup_call =
                layout.latest_steam ? latest_file_size_cleanup_call
                                    : legacy_file_size_cleanup_call;
            const auto& stock_synchronous_finalize_call =
                layout.latest_steam ? latest_synchronous_finalize_call
                                    : legacy_synchronous_finalize_call;
            const auto& stock_synchronous_loose_finalize_call =
                layout.latest_steam ? latest_synchronous_loose_finalize_call
                                    : legacy_synchronous_loose_finalize_call;
            const auto& stock_qcmp_failure_copy_call =
                layout.latest_steam ? latest_qcmp_failure_copy_call
                                    : legacy_qcmp_failure_copy_call;
            const auto& stock_compressed_xml_allocation_call =
                layout.latest_steam ? latest_compressed_xml_allocation_call
                                    : legacy_compressed_xml_allocation_call;
            std::memcpy(sampler_builder_site,
                        texture_filtering::kSamplerBuilderPrologue.data(),
                        texture_filtering::kSamplerBuilderPrologue.size());
            std::memcpy(
                sampler_builder_site +
                    texture_filtering::kForceBranchPrefixOffset,
                texture_filtering::kForceBranchPrefix.data(),
                texture_filtering::kForceBranchPrefix.size());
            std::memcpy(force_anisotropic_filtering_site,
                        texture_filtering::kStockTrilinearInstruction.data(),
                        texture_filtering::kStockTrilinearInstruction.size());
            std::memcpy(
                sampler_builder_site +
                    texture_filtering::kForceBranchSuffixOffset,
                texture_filtering::kForceBranchSuffix.data(),
                texture_filtering::kForceBranchSuffix.size());
            std::memcpy(resolution_site, invalid_resolution.data(),
                        invalid_resolution.size());
            std::memcpy(qpc_clock_site, truncated_qpc_conversion.data(),
                        truncated_qpc_conversion.size());
            std::memcpy(timestamp_open_mode_site,
                        timestamp_open_existing.data(),
                        timestamp_open_existing.size());
            std::memcpy(audio_file_handle_test_site,
                        null_file_handle_test.data(),
                        null_file_handle_test.size());
            std::memcpy(audio_file_mapping_argument_site,
                        mapping_argument_from_rax.data(),
                        mapping_argument_from_rax.size());
            std::memcpy(file_size_combine_site,
                        truncated_file_size_combine.data(),
                        truncated_file_size_combine.size());
            std::memcpy(benchmark_vram_read_site,
                        stock_benchmark_vram_read.data(),
                        stock_benchmark_vram_read.size());
            std::memcpy(vram_validation_call_site,
                        stock_vram_validation_call.data(),
                        stock_vram_validation_call.size());
            std::memcpy(wetness_copy_call_site, stock_wetness_copy_call.data(),
                        stock_wetness_copy_call.size());
            std::memcpy(loaded_chunk_error_call_site,
                        stock_loaded_chunk_error_call.data(),
                        stock_loaded_chunk_error_call.size());
            std::memcpy(loaded_chunk_file_error_call_site,
                        stock_loaded_chunk_file_error_call.data(),
                        stock_loaded_chunk_file_error_call.size());
            std::memcpy(file_size_cleanup_call_site,
                        stock_file_size_cleanup_call.data(),
                        stock_file_size_cleanup_call.size());
            std::memcpy(synchronous_finalize_call_site,
                        stock_synchronous_finalize_call.data(),
                        stock_synchronous_finalize_call.size());
            std::memcpy(synchronous_loose_open_failure_branch_site,
                        synchronous_loose_open_failure_branch.data(),
                        synchronous_loose_open_failure_branch.size());
            std::memcpy(synchronous_loose_finalize_call_site,
                        stock_synchronous_loose_finalize_call.data(),
                        stock_synchronous_loose_finalize_call.size());
            std::memcpy(synchronous_loose_invalid_size_state_site,
                        synchronous_loose_invalid_size_state.data(),
                        synchronous_loose_invalid_size_state.size());
            std::memcpy(qcmp_failure_copy_call_site,
                        stock_qcmp_failure_copy_call.data(),
                        stock_qcmp_failure_copy_call.size());
            std::memcpy(compressed_xml_allocation_call_site,
                        stock_compressed_xml_allocation_call.data(),
                        stock_compressed_xml_allocation_call.size());
            std::memcpy(compressed_xml_finalize_site,
                        compressed_xml_finalize.data(),
                        compressed_xml_finalize.size());
            std::memcpy(raw_mouse_site, raw_mouse_signature.data(),
                        raw_mouse_signature.size());
            constexpr std::array<std::uint8_t, 2> raw_mouse_state_store{0x88,
                                                                        0x0D};
            std::memcpy(raw_mouse_site + raw_mouse_signature.size(),
                        raw_mouse_state_store.data(),
                        raw_mouse_state_store.size());
            const auto raw_mouse_state_displacement = static_cast<std::int32_t>(
                raw_mouse_state -
                (raw_mouse_site + raw_mouse_signature.size() + 6));
            std::memcpy(raw_mouse_site + raw_mouse_signature.size() +
                            raw_mouse_state_store.size(),
                        &raw_mouse_state_displacement,
                        sizeof(raw_mouse_state_displacement));
            *raw_mouse_state = 0;
            std::memcpy(controller_response_site,
                        controller_response_signature.data(),
                        controller_response_signature.size());
            std::memcpy(mouse_camera_site, mouse_camera_signature.data(),
                        mouse_camera_signature.size());
            *vram_validation_function = 0xC3;
            *loaded_chunk_error_logger = 0xC3;
            *loaded_chunk_file_error_logger = 0xC3;
            *release_resource_waiters = 0xC3;
            *file_size_epilogue = 0xC3;
            *resource_finalize = 0xC3;
            *synchronous_loose_open_failure_epilogue = 0xC3;
            constexpr std::array<std::uint8_t, 4> test_buffer_copy{
                0xC6, 0x01, 0x7F, 0xC3};  // mov byte ptr [rcx], 7Fh; ret
            std::memcpy(buffer_copy, test_buffer_copy.data(),
                        test_buffer_copy.size());
            constexpr std::array<std::uint8_t, 4> test_resource_allocator{
                0x48, 0x8B, 0xC1, 0xC3};  // mov rax, rcx; ret
            constexpr std::array<std::uint8_t, 4> test_resource_free{
                0xC6, 0x02, 0x7F, 0xC3};  // mov byte ptr [rdx], 7Fh; ret
            std::memcpy(resource_allocator, test_resource_allocator.data(),
                        test_resource_allocator.size());
            std::memcpy(resource_free, test_resource_free.data(),
                        test_resource_free.size());
            *compressed_xml_cleanup = 0xC3;
            *resource_work_pending = 0;
            *resource_allocator_instance = image;
            DWORD resolution_writable_protection = 0;
            DWORD qpc_writable_protection = 0;
            DWORD timestamp_writable_protection = 0;
            DWORD audio_file_writable_protection = 0;
            DWORD file_size_writable_protection = 0;
            DWORD benchmark_vram_writable_protection = 0;
            DWORD vram_call_writable_protection = 0;
            DWORD vram_function_writable_protection = 0;
            DWORD wetness_copy_writable_protection = 0;
            DWORD resource_callback_writable_protection = 0;
            DWORD resource_error_logger_writable_protection = 0;
            DWORD resource_file_logger_writable_protection = 0;
            DWORD resource_waiter_cleanup_writable_protection = 0;
            DWORD synchronous_finalize_call_writable_protection = 0;
            DWORD synchronous_loose_loader_writable_protection = 0;
            DWORD resource_finalize_writable_protection = 0;
            DWORD qcmp_failure_copy_call_writable_protection = 0;
            DWORD buffer_copy_writable_protection = 0;
            DWORD compressed_xml_loader_writable_protection = 0;
            DWORD resource_allocator_writable_protection = 0;
            DWORD resource_allocator_instance_writable_protection = 0;
            DWORD raw_mouse_writable_protection = 0;
            DWORD controller_response_writable_protection = 0;
            DWORD mouse_camera_writable_protection = 0;
            DWORD sampler_builder_writable_protection = 0;
            const bool made_read_only =
                VirtualProtect(sampler_builder_page, 0x1000,
                               PAGE_EXECUTE_READ,
                               &sampler_builder_writable_protection) != FALSE &&
                VirtualProtect(resolution_page, 0x1000, PAGE_READONLY,
                               &resolution_writable_protection) != FALSE &&
                VirtualProtect(qpc_clock_page, 0x1000, PAGE_READONLY,
                               &qpc_writable_protection) != FALSE &&
                VirtualProtect(timestamp_open_mode_page, 0x1000, PAGE_READONLY,
                               &timestamp_writable_protection) != FALSE &&
                VirtualProtect(audio_file_open_page, 0x1000, PAGE_READONLY,
                               &audio_file_writable_protection) != FALSE &&
                VirtualProtect(file_size_combine_page, 0x1000, PAGE_READONLY,
                               &file_size_writable_protection) != FALSE &&
                VirtualProtect(benchmark_vram_read_page, 0x1000, PAGE_READONLY,
                               &benchmark_vram_writable_protection) != FALSE &&
                VirtualProtect(vram_validation_call_page, 0x1000,
                               PAGE_EXECUTE_READ,
                               &vram_call_writable_protection) != FALSE &&
                VirtualProtect(vram_validation_function_page, 0x1000,
                               PAGE_EXECUTE_READ,
                               &vram_function_writable_protection) != FALSE &&
                VirtualProtect(wetness_copy_page, 0x1000, PAGE_READONLY,
                               &wetness_copy_writable_protection) != FALSE &&
                VirtualProtect(
                    resource_callback_page, 0x1000, PAGE_EXECUTE_READ,
                    &resource_callback_writable_protection) != FALSE &&
                VirtualProtect(
                    resource_error_logger_page, 0x1000, PAGE_EXECUTE_READ,
                    &resource_error_logger_writable_protection) != FALSE &&
                VirtualProtect(
                    resource_file_logger_page, 0x1000, PAGE_EXECUTE_READ,
                    &resource_file_logger_writable_protection) != FALSE &&
                VirtualProtect(
                    resource_waiter_cleanup_page, 0x1000, PAGE_EXECUTE_READ,
                    &resource_waiter_cleanup_writable_protection) != FALSE &&
                VirtualProtect(
                    synchronous_finalize_call_page, 0x1000, PAGE_EXECUTE_READ,
                    &synchronous_finalize_call_writable_protection) != FALSE &&
                VirtualProtect(
                    synchronous_loose_loader_page, 0x1000, PAGE_EXECUTE_READ,
                    &synchronous_loose_loader_writable_protection) != FALSE &&
                VirtualProtect(
                    resource_finalize_page, 0x1000, PAGE_EXECUTE_READ,
                    &resource_finalize_writable_protection) != FALSE &&
                VirtualProtect(
                    qcmp_failure_copy_call_page, 0x1000, PAGE_EXECUTE_READ,
                    &qcmp_failure_copy_call_writable_protection) != FALSE &&
                VirtualProtect(buffer_copy_page, 0x1000, PAGE_EXECUTE_READ,
                               &buffer_copy_writable_protection) != FALSE &&
                VirtualProtect(compressed_xml_loader_page, 0x1000,
                               PAGE_EXECUTE_READ,
                               &compressed_xml_loader_writable_protection) !=
                    FALSE &&
                VirtualProtect(resource_allocator_page, 0x1000,
                               PAGE_EXECUTE_READ,
                               &resource_allocator_writable_protection) !=
                    FALSE &&
                VirtualProtect(resource_allocator_instance_page, 0x1000,
                               PAGE_READONLY,
                               &resource_allocator_instance_writable_protection) !=
                    FALSE &&
                VirtualProtect(raw_mouse_page, 0x1000, PAGE_EXECUTE_READ,
                               &raw_mouse_writable_protection) != FALSE &&
                VirtualProtect(controller_response_page, 0x1000, PAGE_READONLY,
                               &controller_response_writable_protection) !=
                    FALSE &&
                VirtualProtect(mouse_camera_page, 0x1000, PAGE_READONLY,
                               &mouse_camera_writable_protection) != FALSE;
            const bool initialized =
                made_read_only &&
                engine_fixes::InitializeStaticPatches(
                    config, reinterpret_cast<std::uintptr_t>(image),
                    layout.latest_steam, false, {});
            const auto decode_call_target = [](const std::uint8_t* call_site) {
                if (call_site == nullptr || call_site[0] != 0xE8) {
                    return std::uintptr_t{0};
                }
                std::int32_t displacement = 0;
                std::memcpy(&displacement, call_site + 1, sizeof(displacement));
                return reinterpret_cast<std::uintptr_t>(call_site) + 5 +
                       static_cast<std::intptr_t>(displacement);
            };
            const auto decode_zero_jump_target =
                [](const std::uint8_t* jump_site) {
                    if (jump_site == nullptr || jump_site[0] != 0x0F ||
                        jump_site[1] != 0x84) {
                        return std::uintptr_t{0};
                    }
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, jump_site + 2,
                                sizeof(displacement));
                    return reinterpret_cast<std::uintptr_t>(jump_site) + 6 +
                           static_cast<std::intptr_t>(displacement);
                };
            const auto decode_jump_target = [](const std::uint8_t* jump_site) {
                if (jump_site == nullptr || jump_site[0] != 0xE9) {
                    return std::uintptr_t{0};
                }
                std::int32_t displacement = 0;
                std::memcpy(&displacement, jump_site + 1,
                            sizeof(displacement));
                return reinterpret_cast<std::uintptr_t>(jump_site) + 5 +
                       static_cast<std::intptr_t>(displacement);
            };
            const std::uintptr_t loaded_chunk_error_relay =
                decode_call_target(loaded_chunk_error_call_site);
            const std::uintptr_t loaded_chunk_file_error_relay =
                decode_call_target(loaded_chunk_file_error_call_site);
            const std::uintptr_t file_size_cleanup_relay =
                decode_call_target(file_size_cleanup_call_site);
            const std::uintptr_t synchronous_finalize_relay =
                decode_call_target(synchronous_finalize_call_site);
            const std::uintptr_t synchronous_loose_open_relay =
                decode_zero_jump_target(
                    synchronous_loose_open_failure_branch_site);
            const std::uintptr_t synchronous_loose_finalize_relay =
                decode_call_target(synchronous_loose_finalize_call_site);
            const std::uintptr_t synchronous_loose_size_relay =
                decode_call_target(synchronous_loose_invalid_size_state_site);
            const std::uintptr_t qcmp_failure_copy_relay =
                decode_call_target(qcmp_failure_copy_call_site);
            const std::uintptr_t compressed_xml_allocation_relay =
                decode_call_target(compressed_xml_allocation_call_site);
            const std::uintptr_t compressed_xml_finalize_relay =
                decode_jump_target(compressed_xml_finalize_site);
            const auto relay_is_rx = [](std::uintptr_t address) {
                MEMORY_BASIC_INFORMATION region{};
                return address != 0 &&
                       VirtualQuery(reinterpret_cast<const void*>(address),
                                    &region,
                                    sizeof(region)) == sizeof(region) &&
                       region.State == MEM_COMMIT &&
                       region.Protect == PAGE_EXECUTE_READ;
            };
            constexpr std::array<std::uint8_t, 7> resource_log_relay_prefix{
                0x83, 0xBB, 0xB0, 0x00, 0x00, 0x00, 0x01};
            constexpr std::array<std::uint8_t, 4> resource_size_relay_prefix{
                0x33, 0xC0, 0x48, 0x89};
            constexpr std::array<std::uint8_t, 4>
                synchronous_indexed_relay_prefix{0x48, 0x83, 0xB9, 0xA0};
            constexpr std::array<std::uint8_t, 4>
                synchronous_loose_relay_prefix{0x4C, 0x39, 0xB1, 0xA0};
            constexpr std::array<std::uint8_t, 4>
                synchronous_loose_setup_prefix{0x33, 0xC0, 0x48, 0x89};
            constexpr std::array<std::uint8_t, 4> qcmp_copy_guard_prefix{
                0x41, 0x83, 0xF8, 0xFF};
            constexpr std::array<std::uint8_t, 6>
                compressed_xml_allocation_prefix{0x81, 0xF9, 0x80, 0x00,
                                                  0x00, 0x00};
            constexpr std::array<std::uint8_t, 3>
                compressed_xml_finalize_prefix{0x48, 0x85, 0xFF};
            const bool resource_relays_published =
                initialized && relay_is_rx(loaded_chunk_error_relay) &&
                relay_is_rx(loaded_chunk_file_error_relay) &&
                relay_is_rx(file_size_cleanup_relay) &&
                relay_is_rx(synchronous_finalize_relay) &&
                relay_is_rx(synchronous_loose_open_relay) &&
                relay_is_rx(synchronous_loose_finalize_relay) &&
                relay_is_rx(synchronous_loose_size_relay) &&
                relay_is_rx(qcmp_failure_copy_relay) &&
                relay_is_rx(compressed_xml_allocation_relay) &&
                relay_is_rx(compressed_xml_finalize_relay) &&
                std::memcmp(
                    reinterpret_cast<const void*>(loaded_chunk_error_relay),
                    resource_log_relay_prefix.data(),
                    resource_log_relay_prefix.size()) == 0 &&
                std::memcmp(reinterpret_cast<const void*>(
                                loaded_chunk_file_error_relay),
                            resource_log_relay_prefix.data(),
                            resource_log_relay_prefix.size()) == 0 &&
                std::memcmp(
                    reinterpret_cast<const void*>(file_size_cleanup_relay),
                    resource_size_relay_prefix.data(),
                    resource_size_relay_prefix.size()) == 0 &&
                std::memcmp(
                    reinterpret_cast<const void*>(synchronous_finalize_relay),
                    synchronous_indexed_relay_prefix.data(),
                    synchronous_indexed_relay_prefix.size()) == 0 &&
                std::memcmp(
                    reinterpret_cast<const void*>(synchronous_loose_open_relay),
                    synchronous_loose_setup_prefix.data(),
                    synchronous_loose_setup_prefix.size()) == 0 &&
                std::memcmp(reinterpret_cast<const void*>(
                                synchronous_loose_finalize_relay),
                            synchronous_loose_relay_prefix.data(),
                            synchronous_loose_relay_prefix.size()) == 0 &&
                std::memcmp(
                    reinterpret_cast<const void*>(synchronous_loose_size_relay),
                    synchronous_loose_setup_prefix.data(),
                    synchronous_loose_setup_prefix.size()) == 0 &&
                std::memcmp(
                    reinterpret_cast<const void*>(qcmp_failure_copy_relay),
                    qcmp_copy_guard_prefix.data(),
                    qcmp_copy_guard_prefix.size()) == 0 &&
                std::memcmp(reinterpret_cast<const void*>(
                                compressed_xml_allocation_relay),
                            compressed_xml_allocation_prefix.data(),
                            compressed_xml_allocation_prefix.size()) == 0 &&
                std::memcmp(reinterpret_cast<const void*>(
                                compressed_xml_finalize_relay),
                            compressed_xml_finalize_prefix.data(),
                            compressed_xml_finalize_prefix.size()) == 0;
            bool resource_failure_recovery_correct = false;
            if (resource_relays_published) {
                using QcmpFailureCopyGuardFn = void (*)(
                    void*, const void*, std::uint32_t);
                const auto qcmp_copy_guard =
                    reinterpret_cast<QcmpFailureCopyGuardFn>(
                        qcmp_failure_copy_relay);
                std::uint8_t copy_target = 0;
                const std::uint8_t copy_source = 0;
                qcmp_copy_guard(&copy_target, &copy_source, 1);
                const bool normal_qcmp_copy_preserved = copy_target == 0x7F;
                copy_target = 0;
                qcmp_copy_guard(&copy_target, &copy_source,
                                (std::numeric_limits<std::uint32_t>::max)());
                const bool failed_qcmp_copy_rejected = copy_target == 0;
                using CompressedXmlAllocationGuardFn = std::uintptr_t (*)(
                    std::uint32_t, const char*, std::uint32_t);
                const auto allocate_xml =
                    reinterpret_cast<CompressedXmlAllocationGuardFn>(
                        compressed_xml_allocation_relay);
                const bool normal_xml_allocation_preserved =
                    allocate_xml(0x80, "test", 0) == 0x80;
                const bool wrapped_xml_allocation_rejected =
                    allocate_xml(0x7F, "test", 0) == 0;
                bool compressed_xml_finalize_correct = false;

                constexpr std::array<std::uint8_t, 19> rbx_call_wrapper{
                    0x53,                    // push rbx
                    0x48, 0x83, 0xEC, 0x20,  // sub rsp, 20h
                    0x48, 0x8B, 0xD9,        // mov rbx, rcx
                    0x48, 0x8B, 0xC2,        // mov rax, rdx
                    0xFF, 0xD0,              // call rax
                    0x48, 0x83, 0xC4, 0x20,  // add rsp, 20h
                    0x5B,                    // pop rbx
                    0xC3};                   // ret
                constexpr std::array<std::uint8_t, 25> epilogue_call_wrapper{
                    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 28h
                    0x48, 0x8D, 0x05, 0x09, 0x00, 0x00,  // lea rax, [after]
                    0x00, 0x48, 0x89, 0x04, 0x24,        // mov [rsp], rax
                    0x48, 0x8B, 0xC2,                    // mov rax, rdx
                    0xFF, 0xD0,                          // call rax
                    0x48, 0x83, 0xC4, 0x20,              // after: add rsp, 20h
                    0xC3};                               // ret
                constexpr std::array<std::uint8_t, 19> rdi_call_wrapper{
                    0x57,                    // push rdi
                    0x48, 0x83, 0xEC, 0x20,  // sub rsp, 20h
                    0x48, 0x8B, 0xF9,        // mov rdi, rcx
                    0x48, 0x8B, 0xC2,        // mov rax, rdx
                    0xFF, 0xD0,              // call rax
                    0x48, 0x83, 0xC4, 0x20,  // add rsp, 20h
                    0x5F,                    // pop rdi
                    0xC3};                   // ret
                constexpr std::array<std::uint8_t, 19>
                    rsi_expected_call_wrapper{
                        0x56,                    // push rsi
                        0x48, 0x83, 0xEC, 0x20,  // sub rsp, 20h
                        0x49, 0x8B, 0xF0,        // mov rsi, r8
                        0x48, 0x8B, 0xC2,        // mov rax, rdx
                        0xFF, 0xD0,              // call rax
                        0x48, 0x83, 0xC4, 0x20,  // add rsp, 20h
                        0x5E,                    // pop rsi
                        0xC3};                   // ret
                constexpr std::array<std::uint8_t, 21>
                    r14_expected_call_wrapper{
                        0x41, 0x56,              // push r14
                        0x48, 0x83, 0xEC, 0x20,  // sub rsp, 20h
                        0x4D, 0x8B, 0xF0,        // mov r14, r8
                        0x48, 0x8B, 0xC2,        // mov rax, rdx
                        0xFF, 0xD0,              // call rax
                        0x48, 0x83, 0xC4, 0x20,  // add rsp, 20h
                        0x41, 0x5E,              // pop r14
                        0xC3};                   // ret
                constexpr std::array<std::uint8_t, 31>
                    compressed_xml_finalize_wrapper{
                        0x57,                    // push rdi
                        0x53,                    // push rbx
                        0x48, 0x83, 0xEC, 0x28,  // sub rsp, 28h
                        0x48, 0x8B, 0xF9,        // mov rdi, rcx
                        0x48, 0x8B, 0xC2,        // mov rax, rdx
                        0x49, 0x8B, 0xD8,        // mov rbx, r8
                        0x4D, 0x8B, 0xD1,        // mov r10, r9
                        0x41, 0xFF, 0xD2,        // call r10
                        0x48, 0x8B, 0xC7,        // mov rax, rdi
                        0x48, 0x83, 0xC4, 0x28,  // add rsp, 28h
                        0x5B,                    // pop rbx
                        0x5F,                    // pop rdi
                        0xC3};                   // ret
                auto* const wrappers = static_cast<std::uint8_t*>(VirtualAlloc(
                    nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (wrappers != nullptr) {
                    std::memcpy(wrappers, rbx_call_wrapper.data(),
                                rbx_call_wrapper.size());
                    std::memcpy(wrappers + 64, epilogue_call_wrapper.data(),
                                epilogue_call_wrapper.size());
                    std::memcpy(wrappers + 128, rdi_call_wrapper.data(),
                                rdi_call_wrapper.size());
                    std::memcpy(wrappers + 192,
                                rsi_expected_call_wrapper.data(),
                                rsi_expected_call_wrapper.size());
                    std::memcpy(wrappers + 256,
                                r14_expected_call_wrapper.data(),
                                r14_expected_call_wrapper.size());
                    std::memcpy(wrappers + 320,
                                compressed_xml_finalize_wrapper.data(),
                                compressed_xml_finalize_wrapper.size());
                    DWORD wrapper_writable_protection = 0;
                    const bool wrappers_executable =
                        VirtualProtect(wrappers, 0x1000, PAGE_EXECUTE_READ,
                                       &wrapper_writable_protection) != FALSE &&
                        FlushInstructionCache(
                            GetCurrentProcess(), wrappers,
                            320 + compressed_xml_finalize_wrapper.size()) !=
                            FALSE;
                    if (wrappers_executable) {
                        using InvokeCompressedXmlFinalizeFn = void* (*)(
                            void*, std::uint64_t, std::uint64_t, void*);
                        const auto invoke_xml_finalize =
                            reinterpret_cast<InvokeCompressedXmlFinalizeFn>(
                                wrappers + 320);
                        std::array<std::uint8_t, 17> xml_buffer{};
                        xml_buffer.fill(0xAA);
                        const bool xml_success_preserved =
                            invoke_xml_finalize(
                                xml_buffer.data(), 16, 16,
                                reinterpret_cast<void*>(
                                    compressed_xml_finalize_relay)) ==
                                xml_buffer.data() &&
                            xml_buffer[0] == 0xAA && xml_buffer[16] == 0;
                        xml_buffer.fill(0);
                        const bool xml_decode_failure_released =
                            invoke_xml_finalize(
                                xml_buffer.data(),
                                (std::numeric_limits<std::uint64_t>::max)(), 16,
                                reinterpret_cast<void*>(
                                    compressed_xml_finalize_relay)) == nullptr &&
                            xml_buffer[0] == 0x7F;
                        xml_buffer.fill(0);
                        const bool xml_short_decode_released =
                            invoke_xml_finalize(
                                xml_buffer.data(), 15, 16,
                                reinterpret_cast<void*>(
                                    compressed_xml_finalize_relay)) == nullptr &&
                            xml_buffer[0] == 0x7F;
                        const bool xml_null_allocation_rejected =
                            invoke_xml_finalize(
                                nullptr, 0, 0,
                                reinterpret_cast<void*>(
                                    compressed_xml_finalize_relay)) == nullptr;
                        compressed_xml_finalize_correct =
                            xml_success_preserved &&
                            xml_decode_failure_released &&
                            xml_short_decode_released &&
                            xml_null_allocation_rejected;

                        alignas(8) std::array<std::uint8_t, 0xB8> resource{};
                        const auto write_u32 = [&resource](
                                                   std::size_t offset,
                                                   std::uint32_t value) {
                            std::memcpy(resource.data() + offset, &value,
                                        sizeof(value));
                        };
                        const auto read_u32 = [&resource](std::size_t offset) {
                            std::uint32_t value = 0;
                            std::memcpy(&value, resource.data() + offset,
                                        sizeof(value));
                            return value;
                        };
                        const auto write_u64 = [&resource](
                                                   std::size_t offset,
                                                   std::uint64_t value) {
                            std::memcpy(resource.data() + offset, &value,
                                        sizeof(value));
                        };
                        const auto read_u64 = [&resource](std::size_t offset) {
                            std::uint64_t value = 0;
                            std::memcpy(&value, resource.data() + offset,
                                        sizeof(value));
                            return value;
                        };
                        using InvokeRelayFn = void (*)(void*, void*);
                        const auto invoke_rbx =
                            reinterpret_cast<InvokeRelayFn>(wrappers);
                        const auto invoke_epilogue =
                            reinterpret_cast<InvokeRelayFn>(wrappers + 64);
                        const auto invoke_rdi =
                            reinterpret_cast<InvokeRelayFn>(wrappers + 128);
                        using InvokeExpectedRelayFn =
                            void (*)(void*, void*, std::uint64_t);
                        const auto invoke_indexed =
                            reinterpret_cast<InvokeExpectedRelayFn>(wrappers +
                                                                    192);
                        const auto invoke_loose =
                            reinterpret_cast<InvokeExpectedRelayFn>(wrappers +
                                                                    256);

                        write_u32(0xB0, 1);
                        write_u32(0xAC, 0);
                        *resource_work_pending = 0;
                        invoke_rbx(
                            resource.data(),
                            reinterpret_cast<void*>(loaded_chunk_error_relay));
                        const bool chunk_error_recovered =
                            read_u32(0xB0) == 0 && *resource_work_pending == 1;

                        write_u32(0xB0, 1);
                        write_u32(0xAC, 1);
                        *resource_work_pending = 0;
                        invoke_rbx(
                            resource.data(),
                            reinterpret_cast<void*>(loaded_chunk_error_relay));
                        const bool outstanding_operation_preserved =
                            read_u32(0xB0) == 1 && *resource_work_pending == 1;

                        write_u32(0xB0, 2);
                        write_u32(0xAC, 0);
                        *resource_work_pending = 0;
                        invoke_rbx(resource.data(),
                                   reinterpret_cast<void*>(
                                       loaded_chunk_file_error_relay));
                        const bool loaded_state_preserved =
                            read_u32(0xB0) == 2 && *resource_work_pending == 1;

                        write_u32(0xA8, 3);
                        write_u32(0xB0, 1);
                        write_u64(0xA0, 0x1122334455667788ull);
                        *resource_work_pending = 0;
                        invoke_epilogue(
                            resource.data(),
                            reinterpret_cast<void*>(file_size_cleanup_relay));
                        const bool file_size_error_recovered =
                            read_u32(0xA8) == 3 && read_u32(0xB0) == 0 &&
                            read_u64(0xA0) == 0 && *resource_work_pending == 1;

                        write_u32(0xB0, 1);
                        write_u64(0xA0,
                                  (std::numeric_limits<std::uint64_t>::max)());
                        *resource_work_pending = 0;
                        invoke_indexed(
                            resource.data(),
                            reinterpret_cast<void*>(synchronous_finalize_relay),
                            0x40);
                        const bool synchronous_failure_recovered =
                            read_u32(0xB0) == 0 && read_u64(0xA0) == 0 &&
                            *resource_work_pending == 1;

                        write_u32(0xB0, 1);
                        write_u64(0xA0, 0x20);
                        *resource_work_pending = 0;
                        invoke_indexed(
                            resource.data(),
                            reinterpret_cast<void*>(synchronous_finalize_relay),
                            0x40);
                        const bool synchronous_nonfailure_preserved =
                            read_u32(0xB0) == 1 && read_u64(0xA0) == 0x20 &&
                            *resource_work_pending == 0;

                        write_u32(0xB0, 1);
                        write_u64(0xA0, 0x40);
                        *resource_work_pending = 0;
                        invoke_indexed(
                            resource.data(),
                            reinterpret_cast<void*>(synchronous_finalize_relay),
                            0x40);
                        const bool synchronous_success_preserved =
                            read_u32(0xB0) == 1 && read_u64(0xA0) == 0x40 &&
                            *resource_work_pending == 0;

                        write_u32(0xB0, 1);
                        write_u64(0xA0, 0x20);
                        *resource_work_pending = 0;
                        invoke_loose(resource.data(),
                                     reinterpret_cast<void*>(
                                         synchronous_loose_finalize_relay),
                                     0x40);
                        const bool synchronous_loose_short_read_recovered =
                            read_u32(0xB0) == 0 && read_u64(0xA0) == 0 &&
                            *resource_work_pending == 1;

                        write_u32(0xB0, 1);
                        write_u64(0xA0, 0x40);
                        *resource_work_pending = 0;
                        invoke_loose(resource.data(),
                                     reinterpret_cast<void*>(
                                         synchronous_loose_finalize_relay),
                                     0x40);
                        const bool synchronous_loose_success_preserved =
                            read_u32(0xB0) == 1 && read_u64(0xA0) == 0x40 &&
                            *resource_work_pending == 0;

                        write_u32(0xB0, 1);
                        write_u64(0xA0, 0x1122334455667788ull);
                        *resource_work_pending = 0;
                        invoke_rdi(resource.data(),
                                   reinterpret_cast<void*>(
                                       synchronous_loose_open_relay));
                        const bool synchronous_loose_open_recovered =
                            read_u32(0xB0) == 0 && read_u64(0xA0) == 0 &&
                            *resource_work_pending == 1;

                        write_u32(0xB0, 2);
                        write_u64(0xA0, 0x8877665544332211ull);
                        *resource_work_pending = 0;
                        invoke_rdi(resource.data(),
                                   reinterpret_cast<void*>(
                                       synchronous_loose_size_relay));
                        const bool synchronous_loose_size_recovered =
                            read_u32(0xB0) == 0 && read_u64(0xA0) == 0 &&
                            *resource_work_pending == 1;
                        resource_failure_recovery_correct =
                            chunk_error_recovered &&
                            outstanding_operation_preserved &&
                            loaded_state_preserved &&
                            file_size_error_recovered &&
                            synchronous_failure_recovered &&
                            synchronous_nonfailure_preserved &&
                            synchronous_success_preserved &&
                            synchronous_loose_short_read_recovered &&
                            synchronous_loose_success_preserved &&
                            synchronous_loose_open_recovered &&
                            synchronous_loose_size_recovered &&
                            normal_qcmp_copy_preserved &&
                            failed_qcmp_copy_rejected &&
                            normal_xml_allocation_preserved &&
                            wrapped_xml_allocation_rejected &&
                            compressed_xml_finalize_correct;
                    }
                    VirtualFree(wrappers, 0, MEM_RELEASE);
                }
            }
            bool character_surface_relay_published = false;
            bool character_surface_wetness_bridged = false;
            std::uintptr_t character_surface_relay_address = 0;
            if (initialized && wetness_copy_call_site[0] == 0xE8 &&
                std::memcmp(wetness_copy_call_site,
                            stock_wetness_copy_call.data(),
                            stock_wetness_copy_call.size()) != 0) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, wetness_copy_call_site + 1,
                            sizeof(displacement));
                character_surface_relay_address =
                    reinterpret_cast<std::uintptr_t>(wetness_copy_call_site) +
                    5 + static_cast<std::intptr_t>(displacement);
                MEMORY_BASIC_INFORMATION relay_region{};
                constexpr std::array<std::uint8_t, 3> relay_prefix{0x0F, 0x10,
                                                                   0x02};
                character_surface_relay_published =
                    VirtualQuery(reinterpret_cast<const void*>(
                                     character_surface_relay_address),
                                 &relay_region, sizeof(relay_region)) ==
                        sizeof(relay_region) &&
                    relay_region.State == MEM_COMMIT &&
                    relay_region.Protect == PAGE_EXECUTE_READ &&
                    std::memcmp(reinterpret_cast<const void*>(
                                    character_surface_relay_address),
                                relay_prefix.data(), relay_prefix.size()) == 0;
            }
            if (character_surface_relay_published) {
                alignas(16) std::array<std::uint8_t, 0x40> source{};
                alignas(16) std::array<std::uint8_t, 0x40> destination{};
                for (std::size_t index = 0; index < source.size(); ++index) {
                    source[index] = static_cast<std::uint8_t>(index * 3u + 1u);
                }
                constexpr std::uint32_t live_wetness = 0x3F400000u;
                constexpr std::uint32_t live_sweat = 0x3F000000u;
                constexpr std::uint32_t unrelated_wetness_slot = 0x3F800000u;
                constexpr std::uint32_t unrelated_sweat_slot = 0x3E800000u;
                std::memcpy(source.data() + 0x24, &live_wetness,
                            sizeof(live_wetness));
                std::memcpy(source.data() + 0x28, &live_sweat,
                            sizeof(live_sweat));
                std::memcpy(source.data() + 0x38, &unrelated_wetness_slot,
                            sizeof(unrelated_wetness_slot));
                std::memcpy(source.data() + 0x3C, &unrelated_sweat_slot,
                            sizeof(unrelated_sweat_slot));

                using CharacterSurfaceCopyFn =
                    void* (*)(void*, const void*, std::uint32_t);
                const auto copy = reinterpret_cast<CharacterSurfaceCopyFn>(
                    character_surface_relay_address);
                void* const result =
                    copy(destination.data(), source.data(), 0x40);
                auto expected_destination = source;
                std::memcpy(expected_destination.data() + 0x38, &live_wetness,
                            sizeof(live_wetness));
                character_surface_wetness_bridged =
                    result == destination.data() &&
                    destination == expected_destination;
            }
            bool vram_capacity_relay_published = false;
            bool vram_capacity_full_width = false;
            std::uintptr_t vram_capacity_relay_address = 0;
            if (initialized && benchmark_vram_read_site[0] == 0xE8 &&
                std::memcmp(benchmark_vram_read_site,
                            stock_benchmark_vram_read.data(),
                            stock_benchmark_vram_read.size()) != 0) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, benchmark_vram_read_site + 1,
                            sizeof(displacement));
                vram_capacity_relay_address =
                    reinterpret_cast<std::uintptr_t>(benchmark_vram_read_site) +
                    5 + static_cast<std::intptr_t>(displacement);
                MEMORY_BASIC_INFORMATION relay_region{};
                constexpr std::array<std::uint8_t, 4> relay_prefix{0x51, 0x52,
                                                                   0x48, 0xB8};
                vram_capacity_relay_published =
                    VirtualQuery(reinterpret_cast<const void*>(
                                     vram_capacity_relay_address),
                                 &relay_region, sizeof(relay_region)) ==
                        sizeof(relay_region) &&
                    relay_region.State == MEM_COMMIT &&
                    relay_region.Protect == PAGE_EXECUTE_READ &&
                    std::memcmp(reinterpret_cast<const void*>(
                                    vram_capacity_relay_address),
                                relay_prefix.data(), relay_prefix.size()) == 0;
            }
            if (vram_capacity_relay_published) {
                constexpr std::size_t adapter_record_size = 0x150;
                constexpr std::size_t dedicated_memory_offset = 0x110;
                constexpr std::size_t interface_offset = 0x138;
                constexpr std::uint64_t full_capacity_bytes = 12'668'170'240ull;
                constexpr std::uint32_t truncated_capacity_bytes =
                    static_cast<std::uint32_t>(full_capacity_bytes);
                std::array<std::uint8_t, adapter_record_size * 2>
                    adapter_records{};
                void* const first_adapter =
                    reinterpret_cast<void*>(std::uintptr_t{0x1111});
                void* const selected_adapter =
                    reinterpret_cast<void*>(std::uintptr_t{0x2222});
                std::memcpy(adapter_records.data() + interface_offset,
                            &first_adapter, sizeof(first_adapter));
                std::memcpy(adapter_records.data() + adapter_record_size +
                                interface_offset,
                            &selected_adapter, sizeof(selected_adapter));
                std::memcpy(adapter_records.data() + adapter_record_size +
                                dedicated_memory_offset,
                            &full_capacity_bytes, sizeof(full_capacity_bytes));

                *reinterpret_cast<std::uint32_t*>(image_bytes + 0x0243A3A0) = 2;
                *reinterpret_cast<std::uint8_t**>(image_bytes + 0x0243A3A8) =
                    adapter_records.data();
                *reinterpret_cast<void**>(image_bytes + 0x02439AF8) =
                    selected_adapter;
                *reinterpret_cast<std::uint32_t*>(image_bytes + 0x02439BB0) =
                    truncated_capacity_bytes;
                using ReadVramCapacityFn = std::uint32_t (*)();
                const auto read_capacity = reinterpret_cast<ReadVramCapacityFn>(
                    vram_capacity_relay_address);
                const bool selected_value_correct =
                    read_capacity() ==
                    static_cast<std::uint32_t>(full_capacity_bytes >> 20);

                *reinterpret_cast<void**>(image_bytes + 0x02439AF8) =
                    reinterpret_cast<void*>(std::uintptr_t{0x3333});
                const bool fallback_value_correct =
                    read_capacity() == (truncated_capacity_bytes >> 20);
                vram_capacity_full_width =
                    selected_value_correct && fallback_value_correct;
            }
            bool vram_relay_published = false;
            std::uintptr_t vram_relay_address = 0;
            if (initialized && vram_validation_call_site[0] == 0xE8 &&
                std::memcmp(vram_validation_call_site,
                            stock_vram_validation_call.data(),
                            stock_vram_validation_call.size()) != 0) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, vram_validation_call_site + 1,
                            sizeof(displacement));
                vram_relay_address = reinterpret_cast<std::uintptr_t>(
                                         vram_validation_call_site) +
                                     5 +
                                     static_cast<std::intptr_t>(displacement);
                MEMORY_BASIC_INFORMATION relay_region{};
                constexpr std::array<std::uint8_t, 6> relay_prefix{
                    0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8};
                vram_relay_published =
                    VirtualQuery(
                        reinterpret_cast<const void*>(vram_relay_address),
                        &relay_region,
                        sizeof(relay_region)) == sizeof(relay_region) &&
                    relay_region.State == MEM_COMMIT &&
                    relay_region.Protect == PAGE_EXECUTE_READ &&
                    std::memcmp(
                        reinterpret_cast<const void*>(vram_relay_address),
                        relay_prefix.data(), relay_prefix.size()) == 0;
            }
            bool vram_lock_balanced = false;
            if (vram_relay_published) {
                constexpr std::array<std::uint8_t, 19> relay_wrapper{
                    0x53,                    // push rbx
                    0x48, 0x83, 0xEC, 0x20,  // sub rsp, 20h
                    0x48, 0x8B, 0xD9,        // mov rbx, rcx
                    0x48, 0x8B, 0xC2,        // mov rax, rdx
                    0xFF, 0xD0,              // call rax
                    0x48, 0x83, 0xC4, 0x20,  // add rsp, 20h
                    0x5B,                    // pop rbx
                    0xC3};                   // ret
                void* const wrapper = VirtualAlloc(
                    nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (wrapper != nullptr) {
                    std::memcpy(wrapper, relay_wrapper.data(),
                                relay_wrapper.size());
                    DWORD wrapper_writable_protection = 0;
                    const bool wrapper_executable =
                        VirtualProtect(wrapper, 0x1000, PAGE_EXECUTE_READ,
                                       &wrapper_writable_protection) != FALSE &&
                        FlushInstructionCache(GetCurrentProcess(), wrapper,
                                              relay_wrapper.size()) != FALSE;
                    if (wrapper_executable) {
                        alignas(CRITICAL_SECTION)
                            std::array<std::uint8_t,
                                       0x10 + sizeof(CRITICAL_SECTION)>
                                pool{};
                        auto* const critical_section =
                            reinterpret_cast<CRITICAL_SECTION*>(pool.data() +
                                                                0x10);
                        InitializeCriticalSection(critical_section);
                        EnterCriticalSection(critical_section);
                        using InvokeRelayFn = void (*)(void*, void*);
                        reinterpret_cast<InvokeRelayFn>(wrapper)(
                            pool.data(),
                            reinterpret_cast<void*>(vram_relay_address));
                        bool acquired_from_other_thread = false;
                        std::thread probe([&]() {
                            if (TryEnterCriticalSection(critical_section) !=
                                FALSE) {
                                acquired_from_other_thread = true;
                                LeaveCriticalSection(critical_section);
                            }
                        });
                        probe.join();
                        if (!acquired_from_other_thread) {
                            LeaveCriticalSection(critical_section);
                        }
                        DeleteCriticalSection(critical_section);
                        vram_lock_balanced = acquired_from_other_thread;
                    }
                    VirtualFree(wrapper, 0, MEM_RELEASE);
                }
            }
            auto expected_controller_response = controller_response_signature;
            const float left_deadzone = 0.0f;
            const float left_scale = 1.0f;
            const float right_deadzone = 0.12f;
            const float right_scale = 1.0f / (1.0f - right_deadzone);
            std::memcpy(expected_controller_response.data() + 8, &left_deadzone,
                        sizeof(left_deadzone));
            std::memcpy(expected_controller_response.data() + 12,
                        &right_deadzone, sizeof(right_deadzone));
            std::memcpy(expected_controller_response.data() + 20, &left_scale,
                        sizeof(left_scale));
            std::memcpy(expected_controller_response.data() + 24, &right_scale,
                        sizeof(right_scale));
            auto expected_mouse_camera = mouse_camera_signature;
            constexpr float immediate_mouse_drain_rate = 1.0e9f;
            std::memcpy(expected_mouse_camera.data() + 16,
                        &immediate_mouse_drain_rate,
                        sizeof(immediate_mouse_drain_rate));
            std::memcpy(expected_mouse_camera.data() + 20,
                        &immediate_mouse_drain_rate,
                        sizeof(immediate_mouse_drain_rate));
            const bool corrected =
                initialized &&
                std::memcmp(
                    sampler_builder_site,
                    texture_filtering::kSamplerBuilderPrologue.data(),
                    texture_filtering::kSamplerBuilderPrologue.size()) == 0 &&
                std::memcmp(
                    sampler_builder_site +
                        texture_filtering::kForceBranchPrefixOffset,
                    texture_filtering::kForceBranchPrefix.data(),
                    texture_filtering::kForceBranchPrefix.size()) == 0 &&
                std::memcmp(
                    force_anisotropic_filtering_site,
                    texture_filtering::kForcedAnisotropicInstruction.data(),
                    texture_filtering::kForcedAnisotropicInstruction.size()) ==
                    0 &&
                std::memcmp(
                    sampler_builder_site +
                        texture_filtering::kForceBranchSuffixOffset,
                    texture_filtering::kForceBranchSuffix.data(),
                    texture_filtering::kForceBranchSuffix.size()) == 0 &&
                std::memcmp(resolution_site, valid_resolution.data(),
                            valid_resolution.size()) == 0 &&
                std::memcmp(qpc_clock_site, full_qpc_conversion.data(),
                            full_qpc_conversion.size()) == 0 &&
                std::memcmp(timestamp_open_mode_site,
                            timestamp_open_always.data(),
                            timestamp_open_always.size()) == 0 &&
                std::memcmp(audio_file_handle_test_site,
                            invalid_file_handle_test.data(),
                            invalid_file_handle_test.size()) == 0 &&
                std::memcmp(audio_file_mapping_argument_site,
                            mapping_argument_from_rbx.data(),
                            mapping_argument_from_rbx.size()) == 0 &&
                std::memcmp(file_size_combine_site,
                            full_file_size_combine.data(),
                            full_file_size_combine.size()) == 0 &&
                std::memcmp(raw_mouse_site + 7, forced_raw_mouse_read.data(),
                            forced_raw_mouse_read.size()) == 0 &&
                *raw_mouse_state == 1 &&
                std::memcmp(controller_response_site,
                            expected_controller_response.data(),
                            expected_controller_response.size()) == 0 &&
                std::memcmp(mouse_camera_site, expected_mouse_camera.data(),
                            expected_mouse_camera.size()) == 0 &&
                resource_failure_recovery_correct &&
                character_surface_relay_published &&
                character_surface_wetness_bridged &&
                 vram_capacity_relay_published && vram_capacity_full_width &&
                 vram_relay_published && vram_lock_balanced;
            Config reconfigured = config;
            reconfigured.controller_left_stick_deadzone = 20;
            reconfigured.controller_right_stick_deadzone = 0;
            const bool reinitialized =
                engine_fixes::InitializeStaticPatches(
                    reconfigured,
                    reinterpret_cast<std::uintptr_t>(image),
                    layout.latest_steam,
                    false,
                    {});
            auto reconfigured_controller_response =
                controller_response_signature;
            const float reconfigured_left_deadzone = 0.20f;
            const float reconfigured_left_scale =
                1.0f / (1.0f - reconfigured_left_deadzone);
            const float reconfigured_right_deadzone = 0.0f;
            const float reconfigured_right_scale = 1.0f;
            std::memcpy(reconfigured_controller_response.data() + 8,
                        &reconfigured_left_deadzone,
                        sizeof(reconfigured_left_deadzone));
            std::memcpy(reconfigured_controller_response.data() + 12,
                        &reconfigured_right_deadzone,
                        sizeof(reconfigured_right_deadzone));
            std::memcpy(reconfigured_controller_response.data() + 20,
                        &reconfigured_left_scale,
                        sizeof(reconfigured_left_scale));
            std::memcpy(reconfigured_controller_response.data() + 24,
                        &reconfigured_right_scale,
                        sizeof(reconfigured_right_scale));
            const bool reconfigured_in_place =
                reinitialized &&
                std::memcmp(controller_response_site,
                            reconfigured_controller_response.data(),
                            reconfigured_controller_response.size()) == 0;
            const bool shut_down = engine_fixes::ShutdownStaticPatches();
            const bool restored =
                shut_down &&
                std::memcmp(
                    force_anisotropic_filtering_site,
                    texture_filtering::kStockTrilinearInstruction.data(),
                    texture_filtering::kStockTrilinearInstruction.size()) ==
                    0 &&
                std::memcmp(resolution_site, invalid_resolution.data(),
                            invalid_resolution.size()) == 0 &&
                std::memcmp(qpc_clock_site, truncated_qpc_conversion.data(),
                            truncated_qpc_conversion.size()) == 0 &&
                std::memcmp(timestamp_open_mode_site,
                            timestamp_open_existing.data(),
                            timestamp_open_existing.size()) == 0 &&
                std::memcmp(audio_file_handle_test_site,
                            null_file_handle_test.data(),
                            null_file_handle_test.size()) == 0 &&
                std::memcmp(audio_file_mapping_argument_site,
                            mapping_argument_from_rax.data(),
                            mapping_argument_from_rax.size()) == 0 &&
                std::memcmp(file_size_combine_site,
                            truncated_file_size_combine.data(),
                            truncated_file_size_combine.size()) == 0 &&
                std::memcmp(benchmark_vram_read_site,
                            stock_benchmark_vram_read.data(),
                            stock_benchmark_vram_read.size()) == 0 &&
                std::memcmp(vram_validation_call_site,
                            stock_vram_validation_call.data(),
                            stock_vram_validation_call.size()) == 0 &&
                std::memcmp(wetness_copy_call_site,
                            stock_wetness_copy_call.data(),
                            stock_wetness_copy_call.size()) == 0 &&
                std::memcmp(loaded_chunk_error_call_site,
                            stock_loaded_chunk_error_call.data(),
                            stock_loaded_chunk_error_call.size()) == 0 &&
                std::memcmp(loaded_chunk_file_error_call_site,
                            stock_loaded_chunk_file_error_call.data(),
                            stock_loaded_chunk_file_error_call.size()) == 0 &&
                std::memcmp(file_size_cleanup_call_site,
                            stock_file_size_cleanup_call.data(),
                            stock_file_size_cleanup_call.size()) == 0 &&
                std::memcmp(synchronous_finalize_call_site,
                            stock_synchronous_finalize_call.data(),
                            stock_synchronous_finalize_call.size()) == 0 &&
                std::memcmp(synchronous_loose_open_failure_branch_site,
                            synchronous_loose_open_failure_branch.data(),
                            synchronous_loose_open_failure_branch.size()) ==
                    0 &&
                std::memcmp(synchronous_loose_finalize_call_site,
                            stock_synchronous_loose_finalize_call.data(),
                            stock_synchronous_loose_finalize_call.size()) ==
                    0 &&
                std::memcmp(synchronous_loose_invalid_size_state_site,
                            synchronous_loose_invalid_size_state.data(),
                            synchronous_loose_invalid_size_state.size()) == 0 &&
                std::memcmp(qcmp_failure_copy_call_site,
                            stock_qcmp_failure_copy_call.data(),
                            stock_qcmp_failure_copy_call.size()) == 0 &&
                std::memcmp(compressed_xml_allocation_call_site,
                            stock_compressed_xml_allocation_call.data(),
                            stock_compressed_xml_allocation_call.size()) == 0 &&
                std::memcmp(compressed_xml_finalize_site,
                            compressed_xml_finalize.data(),
                            compressed_xml_finalize.size()) == 0 &&
                std::memcmp(raw_mouse_site, raw_mouse_signature.data(),
                            raw_mouse_signature.size()) == 0 &&
                *raw_mouse_state == 0 &&
                std::memcmp(controller_response_site,
                            controller_response_signature.data(),
                            controller_response_signature.size()) == 0 &&
                std::memcmp(mouse_camera_site, mouse_camera_signature.data(),
                            mouse_camera_signature.size()) == 0;

            const auto exercise_character_surface_mode =
                [&](bool bridge_wetness, bool sweat_enabled) {
                    Config mode_config = config;
                    mode_config.restore_character_wetness = bridge_wetness;
                    mode_config.restore_character_sweat = sweat_enabled;
                    const bool mode_initialized =
                        engine_fixes::InitializeStaticPatches(
                            mode_config,
                            reinterpret_cast<std::uintptr_t>(image),
                            layout.latest_steam, false, {});

                    bool mode_correct =
                        mode_initialized && !bridge_wetness &&
                        std::memcmp(wetness_copy_call_site,
                                    stock_wetness_copy_call.data(),
                                    stock_wetness_copy_call.size()) == 0;
                    if (mode_initialized && bridge_wetness &&
                        wetness_copy_call_site[0] == 0xE8 &&
                        std::memcmp(wetness_copy_call_site,
                                    stock_wetness_copy_call.data(),
                                    stock_wetness_copy_call.size()) != 0) {
                        std::int32_t displacement = 0;
                        std::memcpy(&displacement, wetness_copy_call_site + 1,
                                    sizeof(displacement));
                        const std::uintptr_t relay_address =
                            reinterpret_cast<std::uintptr_t>(
                                wetness_copy_call_site) +
                            5 + static_cast<std::intptr_t>(displacement);

                        alignas(16) std::array<std::uint8_t, 0x40> source{};
                        alignas(16) std::array<std::uint8_t, 0x40>
                            destination{};
                        for (std::size_t index = 0; index < source.size();
                             ++index) {
                            source[index] =
                                static_cast<std::uint8_t>(index * 5u + 3u);
                        }
                        constexpr std::uint32_t live_wetness = 0x3F400000u;
                        constexpr std::uint32_t live_sweat = 0x3F000000u;
                        constexpr std::uint32_t unrelated_wetness_slot =
                            0x3F800000u;
                        constexpr std::uint32_t unrelated_sweat_slot =
                            0x3E800000u;
                        std::memcpy(source.data() + 0x24, &live_wetness,
                                    sizeof(live_wetness));
                        std::memcpy(source.data() + 0x28, &live_sweat,
                                    sizeof(live_sweat));
                        std::memcpy(source.data() + 0x38,
                                    &unrelated_wetness_slot,
                                    sizeof(unrelated_wetness_slot));
                        std::memcpy(source.data() + 0x3C, &unrelated_sweat_slot,
                                    sizeof(unrelated_sweat_slot));

                        using CharacterSurfaceCopyFn =
                            void* (*)(void*, const void*, std::uint32_t);
                        const auto copy =
                            reinterpret_cast<CharacterSurfaceCopyFn>(
                                relay_address);
                        void* const result =
                            copy(destination.data(), source.data(), 0x40);
                        auto expected = source;
                        if (bridge_wetness) {
                            std::memcpy(expected.data() + 0x38, &live_wetness,
                                        sizeof(live_wetness));
                        }
                        mode_correct = result == destination.data() &&
                                       destination == expected;
                    }

                    const bool mode_shutdown =
                        engine_fixes::ShutdownStaticPatches();
                    const bool call_restored =
                        std::memcmp(wetness_copy_call_site,
                                    stock_wetness_copy_call.data(),
                                    stock_wetness_copy_call.size()) == 0;
                    return mode_initialized && mode_correct && mode_shutdown &&
                           call_restored;
                };
            const bool wetness_only_bridge =
                restored && exercise_character_surface_mode(true, false);
            const bool sweat_only_preserves_stock_copy =
                wetness_only_bridge &&
                exercise_character_surface_mode(false, true);

            MEMORY_BASIC_INFORMATION sampler_builder_region{};
            MEMORY_BASIC_INFORMATION resolution_region{};
            MEMORY_BASIC_INFORMATION qpc_region{};
            MEMORY_BASIC_INFORMATION timestamp_region{};
            MEMORY_BASIC_INFORMATION audio_file_region{};
            MEMORY_BASIC_INFORMATION file_size_region{};
            MEMORY_BASIC_INFORMATION benchmark_vram_region{};
            MEMORY_BASIC_INFORMATION vram_call_region{};
            MEMORY_BASIC_INFORMATION wetness_copy_region{};
            MEMORY_BASIC_INFORMATION resource_callback_region{};
            MEMORY_BASIC_INFORMATION synchronous_finalize_call_region{};
            MEMORY_BASIC_INFORMATION raw_mouse_region{};
            MEMORY_BASIC_INFORMATION controller_response_region{};
            MEMORY_BASIC_INFORMATION mouse_camera_region{};
            const bool protection_restored =
                VirtualQuery(sampler_builder_site, &sampler_builder_region,
                             sizeof(sampler_builder_region)) ==
                    sizeof(sampler_builder_region) &&
                sampler_builder_region.Protect == PAGE_EXECUTE_READ &&
                VirtualQuery(resolution_site, &resolution_region,
                             sizeof(resolution_region)) ==
                    sizeof(resolution_region) &&
                resolution_region.Protect == PAGE_READONLY &&
                VirtualQuery(qpc_clock_site, &qpc_region, sizeof(qpc_region)) ==
                    sizeof(qpc_region) &&
                qpc_region.Protect == PAGE_READONLY &&
                VirtualQuery(timestamp_open_mode_site, &timestamp_region,
                             sizeof(timestamp_region)) ==
                    sizeof(timestamp_region) &&
                timestamp_region.Protect == PAGE_READONLY &&
                VirtualQuery(audio_file_handle_test_site, &audio_file_region,
                             sizeof(audio_file_region)) ==
                    sizeof(audio_file_region) &&
                audio_file_region.Protect == PAGE_READONLY &&
                VirtualQuery(file_size_combine_site, &file_size_region,
                             sizeof(file_size_region)) ==
                    sizeof(file_size_region) &&
                file_size_region.Protect == PAGE_READONLY &&
                VirtualQuery(benchmark_vram_read_site, &benchmark_vram_region,
                             sizeof(benchmark_vram_region)) ==
                    sizeof(benchmark_vram_region) &&
                benchmark_vram_region.Protect == PAGE_READONLY &&
                VirtualQuery(vram_validation_call_site, &vram_call_region,
                             sizeof(vram_call_region)) ==
                    sizeof(vram_call_region) &&
                vram_call_region.Protect == PAGE_EXECUTE_READ &&
                VirtualQuery(wetness_copy_call_site, &wetness_copy_region,
                             sizeof(wetness_copy_region)) ==
                    sizeof(wetness_copy_region) &&
                wetness_copy_region.Protect == PAGE_READONLY &&
                VirtualQuery(loaded_chunk_error_call_site,
                             &resource_callback_region,
                             sizeof(resource_callback_region)) ==
                    sizeof(resource_callback_region) &&
                resource_callback_region.Protect == PAGE_EXECUTE_READ &&
                VirtualQuery(synchronous_finalize_call_site,
                             &synchronous_finalize_call_region,
                             sizeof(synchronous_finalize_call_region)) ==
                    sizeof(synchronous_finalize_call_region) &&
                synchronous_finalize_call_region.Protect == PAGE_EXECUTE_READ &&
                VirtualQuery(raw_mouse_site, &raw_mouse_region,
                             sizeof(raw_mouse_region)) ==
                    sizeof(raw_mouse_region) &&
                raw_mouse_region.Protect == PAGE_EXECUTE_READ &&
                VirtualQuery(controller_response_site,
                             &controller_response_region,
                             sizeof(controller_response_region)) ==
                    sizeof(controller_response_region) &&
                controller_response_region.Protect == PAGE_READONLY &&
                VirtualQuery(mouse_camera_site, &mouse_camera_region,
                             sizeof(mouse_camera_region)) ==
                    sizeof(mouse_camera_region) &&
                mouse_camera_region.Protect == PAGE_READONLY;

            // MinHook owns the builder entry before static patches commit.
            // Prove that only the explicit, successfully prevalidated path may
            // bypass a now-overwritten prologue, while the exact branch bytes
            // and Registry-owned instruction remain mandatory.
            bool prevalidated_builder_path = false;
            DWORD builder_test_protection = 0;
            if (VirtualProtect(sampler_builder_page, 0x1000,
                               PAGE_EXECUTE_READWRITE,
                               &builder_test_protection) != FALSE) {
                const std::uint8_t stock_entry_byte = sampler_builder_site[0];
                sampler_builder_site[0] = 0xE9;
                FlushInstructionCache(GetCurrentProcess(),
                                      sampler_builder_site, 1);
                DWORD ignored_protection = 0;
                const bool resealed =
                    VirtualProtect(sampler_builder_page, 0x1000,
                                   builder_test_protection,
                                   &ignored_protection) != FALSE;

                Config anisotropy_only =
                    BuildSafeCompatibilityConfig(Config{});
                anisotropy_only.force_anisotropic_filtering = true;
                const bool rejected_without_prevalidation =
                    resealed && !engine_fixes::InitializeStaticPatches(
                                    anisotropy_only,
                                    reinterpret_cast<std::uintptr_t>(image),
                                    layout.latest_steam, false, {});
                const bool rejected_left_stock =
                    std::memcmp(
                        force_anisotropic_filtering_site,
                        texture_filtering::kStockTrilinearInstruction.data(),
                        texture_filtering::kStockTrilinearInstruction.size()) ==
                    0;
                const bool rejected_shutdown =
                    engine_fixes::ShutdownStaticPatches();

                const bool accepted_with_prevalidation =
                    rejected_shutdown &&
                    engine_fixes::InitializeStaticPatches(
                        anisotropy_only,
                        reinterpret_cast<std::uintptr_t>(image),
                        layout.latest_steam, true, {});
                const bool accepted_forced =
                    std::memcmp(
                        force_anisotropic_filtering_site,
                        texture_filtering::kForcedAnisotropicInstruction.data(),
                        texture_filtering::kForcedAnisotropicInstruction.size()) ==
                    0;
                const bool accepted_shutdown =
                    engine_fixes::ShutdownStaticPatches();
                const bool accepted_restored =
                    std::memcmp(
                        force_anisotropic_filtering_site,
                        texture_filtering::kStockTrilinearInstruction.data(),
                        texture_filtering::kStockTrilinearInstruction.size()) ==
                    0;

                DWORD restore_write_protection = 0;
                const bool reopened =
                    VirtualProtect(sampler_builder_page, 0x1000,
                                   PAGE_EXECUTE_READWRITE,
                                   &restore_write_protection) != FALSE;
                if (reopened) {
                    sampler_builder_site[0] = stock_entry_byte;
                    FlushInstructionCache(GetCurrentProcess(),
                                          sampler_builder_site, 1);
                }
                DWORD final_protection = 0;
                const bool restored_protection =
                    reopened &&
                    VirtualProtect(sampler_builder_page, 0x1000,
                                   builder_test_protection,
                                   &final_protection) != FALSE;
                prevalidated_builder_path =
                    rejected_without_prevalidation && rejected_left_stock &&
                    accepted_with_prevalidation && accepted_forced &&
                    accepted_shutdown && accepted_restored &&
                    restored_protection;
            }
            VirtualFree(image, 0, MEM_RELEASE);

            if (!Expect(
                    corrected && reconfigured_in_place && restored &&
                        wetness_only_bridge &&
                        sweat_only_preserves_stock_copy && protection_restored &&
                        prevalidated_builder_path,
                    "core corrections should reconfigure numeric patch values, "
                    "promote only the verified trilinear sampler branch, bridge "
                    "only wetness, preserve the sweat material slot, accept only "
                    "an explicitly prevalidated detour entry, and roll back on "
                    "both builds")) {
                return EXIT_FAILURE;
            }
        }
    }

    {
        struct ContactPatchLayout {
            bool latest_steam = false;
            std::uintptr_t call_rva = 0;
            std::array<std::uint8_t, 5> stock_call{};
        };
        constexpr std::array layouts{
            ContactPatchLayout{false, 0x005D1B76,
                               {0xE8, 0x21, 0xF0, 0xCD, 0x00}},
            ContactPatchLayout{true, 0x005D1C46,
                               {0xE8, 0xD1, 0xEB, 0xCD, 0x00}},
        };
        constexpr std::array<std::uint8_t, 5> skipped_call{
            0x90, 0x90, 0x90, 0x90, 0x90};

        for (const ContactPatchLayout& layout : layouts) {
            constexpr std::size_t image_size = 0x005D3000;
            constexpr std::uintptr_t page_mask = ~std::uintptr_t{0xFFF};
            void* const image =
                VirtualAlloc(nullptr, image_size, MEM_RESERVE, PAGE_NOACCESS);
            if (!Expect(image != nullptr,
                        "contact-list patch image should reserve")) {
                return EXIT_FAILURE;
            }

            auto* const image_bytes = static_cast<std::uint8_t*>(image);
            auto* const call_page = static_cast<std::uint8_t*>(VirtualAlloc(
                image_bytes + (layout.call_rva & page_mask), 0x1000,
                MEM_COMMIT, PAGE_READWRITE));
            if (!Expect(call_page != nullptr,
                        "contact-list patch page should commit")) {
                VirtualFree(image, 0, MEM_RELEASE);
                return EXIT_FAILURE;
            }

            auto* const call_site = image_bytes + layout.call_rva;
            std::memcpy(call_site, layout.stock_call.data(),
                        layout.stock_call.size());
            DWORD writable_protection = 0;
            const bool made_executable =
                VirtualProtect(call_page, 0x1000, PAGE_EXECUTE_READ,
                               &writable_protection) != FALSE;

            Config config = BuildSafeCompatibilityConfig(Config{});
            config.fix_contact_list_overflow = true;
            const bool initialized =
                made_executable && engine_fixes::InitializeStaticPatches(
                                       config,
                                       reinterpret_cast<std::uintptr_t>(image),
                                       layout.latest_steam, false, {});
            const bool skipped =
                initialized &&
                std::memcmp(call_site, skipped_call.data(),
                            skipped_call.size()) == 0;
            const bool shut_down = engine_fixes::ShutdownStaticPatches();
            MEMORY_BASIC_INFORMATION region{};
            const bool restored =
                shut_down &&
                std::memcmp(call_site, layout.stock_call.data(),
                            layout.stock_call.size()) == 0 &&
                VirtualQuery(call_site, &region, sizeof(region)) ==
                    sizeof(region) &&
                region.Protect == PAGE_EXECUTE_READ;
            DWORD executable_protection = 0;
            bool unexpected_rejected = false;
            if (restored &&
                VirtualProtect(call_page, 0x1000, PAGE_READWRITE,
                               &executable_protection) != FALSE) {
                constexpr std::array<std::uint8_t, 5> unexpected_call{
                    0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
                std::memcpy(call_site, unexpected_call.data(),
                            unexpected_call.size());
                DWORD temporary_writable_protection = 0;
                const bool sealed =
                    VirtualProtect(call_page, 0x1000, PAGE_EXECUTE_READ,
                                   &temporary_writable_protection) != FALSE;
                const bool rejected =
                    sealed && !engine_fixes::InitializeStaticPatches(
                                  config,
                                  reinterpret_cast<std::uintptr_t>(image),
                                  layout.latest_steam, false, {});
                unexpected_rejected =
                    rejected &&
                    std::memcmp(call_site, unexpected_call.data(),
                                unexpected_call.size()) == 0 &&
                    engine_fixes::ShutdownStaticPatches();
            }
            VirtualFree(image, 0, MEM_RELEASE);

            if (!Expect(skipped,
                        "contact-list overflow fix should skip the dead "
                        "formatter on both layouts")) {
                return EXIT_FAILURE;
            }
            if (!Expect(restored,
                        "contact-list overflow fix should restore stock bytes "
                        "and page protection on both layouts")) {
                return EXIT_FAILURE;
            }
            if (!Expect(unexpected_rejected,
                        "contact-list overflow fix should reject unknown bytes "
                        "without mutation on both layouts")) {
                return EXIT_FAILURE;
            }
        }
    }

    {
        // Execute the exact 14-byte replacement with the register and stack
        // layout used by the game's file-information helper.
        constexpr std::array<std::uint8_t, 48> file_size_wrapper{
            0x55,                          // push rbp
            0x41, 0x56,                    // push r14
            0x48, 0x83, 0xEC, 0x10,        // sub rsp, 10h
            0x89, 0x0C, 0x24,              // mov [rsp], ecx
            0x89, 0x54, 0x24, 0x04,        // mov [rsp+4], edx
            0x48, 0x8D, 0x6C, 0x24, 0xF8,  // lea rbp, [rsp-8]
            0x49, 0x89, 0xE6,              // mov r14, rsp
            0x48, 0x8B, 0x55, 0x08,        // mov rdx, [rbp+8]
            0x48, 0xC1, 0xCA, 0x20,        // ror rdx, 32
            0x49, 0x89, 0x56, 0x08,        // mov [r14+8], rdx
            0x66, 0x90,                    // nop
            0x49, 0x8B, 0x46, 0x08,        // mov rax, [r14+8]
            0x48, 0x83, 0xC4, 0x10,        // add rsp, 10h
            0x41, 0x5E,                    // pop r14
            0x5D,                          // pop rbp
            0xC3};                         // ret
        void* const code = VirtualAlloc(
            nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!Expect(code != nullptr,
                    "file-size combine test code should allocate")) {
            return EXIT_FAILURE;
        }
        std::memcpy(code, file_size_wrapper.data(), file_size_wrapper.size());
        DWORD writable_protection = 0;
        const bool made_executable =
            VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ,
                           &writable_protection) != FALSE &&
            FlushInstructionCache(GetCurrentProcess(), code,
                                  file_size_wrapper.size()) != FALSE;
        using CombineFileSizeFn =
            std::uint64_t (*)(std::uint32_t, std::uint32_t);
        bool combined = false;
        if (made_executable) {
            const auto combine = reinterpret_cast<CombineFileSizeFn>(code);
            combined =
                combine(0, 0xFFFFFFFFu) == 0xFFFFFFFFull &&
                combine(1, 0) == 0x1'00000000ull &&
                combine(1, 2) == 0x1'00000002ull &&
                combine(0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFFFFFFFFFull;
        }
        VirtualFree(code, 0, MEM_RELEASE);
        if (!Expect(made_executable && combined,
                    "file-size replacement should preserve the complete 64-bit "
                    "value")) {
            return EXIT_FAILURE;
        }
    }

    {
        // Invoke the replacement instruction sequence across the first stock
        // overflow boundary. The wrapper adapts the normal Windows x64 calling
        // convention to the Scaleform register state at the patch site.
        constexpr std::array<std::uint8_t, 24> clock_conversion_wrapper{
            0x53,                          // push rbx
            0x48, 0x8B, 0xD9,              // mov rbx, rcx
            0x4C, 0x8B, 0xC2,              // mov r8, rdx
            0x48, 0x8B, 0xC3,              // mov rax, rbx
            0xB9, 0x40, 0x42, 0x0F, 0x00,  // mov ecx, 1000000
            0x48, 0xF7, 0xE1,              // mul rcx
            0x49, 0xF7, 0xF0,              // div r8
            0x90,                          // nop
            0x5B,                          // pop rbx
            0xC3};                         // ret
        void* const code = VirtualAlloc(
            nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!Expect(code != nullptr,
                    "QPC conversion test code should allocate")) {
            return EXIT_FAILURE;
        }
        std::memcpy(code, clock_conversion_wrapper.data(),
                    clock_conversion_wrapper.size());
        DWORD writable_protection = 0;
        const bool made_executable =
            VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ,
                           &writable_protection) != FALSE &&
            FlushInstructionCache(GetCurrentProcess(), code,
                                  clock_conversion_wrapper.size()) != FALSE;
        using ConvertQpcFn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
        constexpr std::uint64_t frequency = 10'000'000;
        constexpr std::uint64_t first_overflow_counter =
            std::numeric_limits<std::uint64_t>::max() / 1'000'000 + 1;
        std::uint64_t before = 0;
        std::uint64_t after = 0;
        if (made_executable) {
            const auto convert = reinterpret_cast<ConvertQpcFn>(code);
            before = convert(first_overflow_counter - 1, frequency);
            after = convert(first_overflow_counter, frequency);
        }
        VirtualFree(code, 0, MEM_RELEASE);
        if (!Expect(made_executable &&
                        before == (first_overflow_counter - 1) / 10 &&
                        after == first_overflow_counter / 10 && after >= before,
                    "QPC conversion should remain monotonic across the stock "
                    "overflow")) {
            return EXIT_FAILURE;
        }
    }

    {
        using spatch::hooks::CutscenePauseState;
        using spatch::hooks::ResolveCutsceneFrameflowDelta;

        const auto no_scope = ResolveCutsceneFrameflowDelta(
            false, CutscenePauseState{}, true, 0.0f, 1.0f / 60.0f);
        if (!Expect(!no_scope.applied_zero_dt_fix,
                    "frameflow zero-dt fix should not apply outside cutscene "
                    "scope")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                no_scope.forwarded_dt == 0.0f,
                "frameflow outside cutscene scope should preserve zero dt")) {
            return EXIT_FAILURE;
        }

        const auto normal_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{}, true, 0.0f, 1.0f / 60.0f);
        if (!Expect(
                normal_fix.applied_zero_dt_fix,
                "frameflow zero-dt fix should apply inside cutscene scope at "
                "60 Hz")) {
            return EXIT_FAILURE;
        }
        if (!Expect(normal_fix.forwarded_dt > 0.015f &&
                        normal_fix.forwarded_dt < 0.019f,
                    "frameflow zero-dt fix should forward the cutscene input "
                    "delta")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!normal_fix.applied_while_game_paused,
                    "normal cutscene zero-dt fix should not mark paused "
                    "override")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!normal_fix.applied_while_simtime_paused,
                    "normal cutscene zero-dt fix should not mark simtime pause "
                    "override")) {
            return EXIT_FAILURE;
        }

        const auto ui_paused_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{true, false}, true, 0.0f, 1.0f / 60.0f);
        if (!Expect(
                !ui_paused_fix.applied_zero_dt_fix,
                "cutscene zero-dt fix should not apply while the UI pause flag "
                "is set")) {
            return EXIT_FAILURE;
        }
        if (!Expect(ui_paused_fix.forwarded_dt == 0.0f,
                    "UI-paused cutscene frameflow should preserve zero dt")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!ui_paused_fix.applied_while_game_paused,
                    "UI-paused cutscene zero-dt path should not mark a pause "
                    "override")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!ui_paused_fix.applied_while_simtime_paused,
                    "UI-paused cutscene zero-dt path should not mark a simtime "
                    "pause override")) {
            return EXIT_FAILURE;
        }

        const auto simtime_paused_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{false, true}, true, 0.0f, 1.0f / 60.0f);
        if (!Expect(simtime_paused_fix.applied_zero_dt_fix,
                    "cutscene zero-dt fix should still apply when only simtime "
                    "pause is set")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                simtime_paused_fix.forwarded_dt > 0.015f &&
                    simtime_paused_fix.forwarded_dt < 0.019f,
                "simtime-paused cutscene frameflow should forward the cutscene "
                "input delta")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!simtime_paused_fix.applied_while_game_paused,
                    "simtime-paused cutscene zero-dt path should not mark a UI "
                    "pause override")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                simtime_paused_fix.applied_while_simtime_paused,
                "simtime-paused cutscene zero-dt path should mark the simtime "
                "pause override")) {
            return EXIT_FAILURE;
        }

        const auto tiny_transition_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{}, true, 1.0f / 600.0f, 1.0f / 60.0f);
        if (!Expect(
                tiny_transition_fix.applied_zero_dt_fix,
                "cutscene zero-dt fix should also normalize tiny transition "
                "deltas")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                tiny_transition_fix.forwarded_dt > 0.015f &&
                    tiny_transition_fix.forwarded_dt < 0.019f,
                "tiny cutscene transition deltas should forward the cutscene "
                "input delta")) {
            return EXIT_FAILURE;
        }

        const auto high_refresh_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{false, false}, true, 0, 0.0f,
            1.0f / 144.0f);
        if (!Expect(
                high_refresh_fix.applied_zero_dt_fix &&
                    high_refresh_fix.forwarded_dt > 0.006f &&
                    high_refresh_fix.forwarded_dt < 0.008f,
                "cutscene zero-dt fix should follow a 144 Hz live cadence")) {
            return EXIT_FAILURE;
        }

        for (int fps = spatch::kCutsceneFpsMin; fps <= spatch::kCutsceneFpsMax;
             ++fps) {
            const float input_dt = 1.0f / static_cast<float>(fps);
            const auto arbitrary_rate = ResolveCutsceneFrameflowDelta(
                true, CutscenePauseState{}, true, fps, 0.0f, input_dt);
            if (!Expect(arbitrary_rate.applied_zero_dt_fix &&
                            std::fabs(arbitrary_rate.forwarded_dt - input_dt) <
                                0.00001f &&
                            !spatch::hooks::IsConfiguredLegacyRateFallback(
                                input_dt, fps),
                        "zero-dt correction should support every integer "
                        "target from 15 "
                        "to 1000")) {
                return EXIT_FAILURE;
            }
        }

        if (!Expect(!spatch::hooks::IsConfiguredLegacyRateFallback(1.0f / 30.0f,
                                                                   15) &&
                        !spatch::hooks::IsConfiguredLegacyRateFallback(
                            1.0f / 60.0f, 15) &&
                        !spatch::hooks::IsConfiguredLegacyRateFallback(
                            1.0f / 30.0f, 30) &&
                        !spatch::hooks::IsConfiguredLegacyRateFallback(
                            1.0f / 60.0f, 30) &&
                        !spatch::hooks::IsConfiguredLegacyRateFallback(
                            1.0f / 60.0f, 45) &&
                        !spatch::hooks::IsConfiguredLegacyRateFallback(
                            1.0f / 60.0f, 60),
                    "canonical rates equal to or faster than a requested "
                    "target are "
                    "not fallbacks")) {
            return EXIT_FAILURE;
        }

        const auto configured_120_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{}, true, 120, 0.0f, 0.0f);
        if (!Expect(configured_120_fix.applied_zero_dt_fix &&
                        configured_120_fix.forwarded_dt > 0.0082f &&
                        configured_120_fix.forwarded_dt < 0.0085f,
                    "configured 120 Hz should synthesize a missing tick")) {
            return EXIT_FAILURE;
        }

        const float remembered_144 =
            spatch::hooks::ResolveTrackedCutsceneBaseDelta(
                0.0f, 1.0f / 144.0f, spatch::kCutsceneFpsAuto);
        const auto auto_both_zero_fix = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{}, true, 0, 0.0f, remembered_144);
        if (!Expect(
                auto_both_zero_fix.applied_zero_dt_fix &&
                    std::fabs(auto_both_zero_fix.forwarded_dt -
                              (1.0f / 144.0f)) < 0.00001f,
                "auto mode should use the same cutscene's last sane cadence "
                "when both inputs are zero")) {
            return EXIT_FAILURE;
        }

        const float remembered_during_30_fallback =
            spatch::hooks::ResolveTrackedCutsceneBaseDelta(
                1.0f / 30.0f, 1.0f / 144.0f, spatch::kCutsceneFpsAuto);
        if (!Expect(
                std::fabs(remembered_during_30_fallback - (1.0f / 144.0f)) <
                    0.00001f,
                "auto mode should not learn a one-frame 30 Hz fallback as its "
                "new cadence")) {
            return EXIT_FAILURE;
        }

        const float configured_45_during_30_fallback =
            spatch::hooks::ResolveTrackedCutsceneBaseDelta(1.0f / 30.0f,
                                                           1.0f / 45.0f, 45);
        const float configured_62_during_60_fallback =
            spatch::hooks::ResolveTrackedCutsceneBaseDelta(1.0f / 60.0f,
                                                           1.0f / 62.0f, 62);
        if (!Expect(
                std::fabs(configured_45_during_30_fallback - (1.0f / 45.0f)) <
                        0.00001f &&
                    std::fabs(configured_62_during_60_fallback -
                              (1.0f / 62.0f)) < 0.00001f,
                "configured 30/60 Hz fallbacks should retain nearby requested "
                "cadences")) {
            return EXIT_FAILURE;
        }

        const auto configured_45_scene_fallback =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 1.0f, 1.0f, 1.0f + (1.0f / 30.0f),
                configured_45_during_30_fallback, 45);
        const auto configured_62_scene_fallback =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 1.0f, 1.0f, 1.0f + (1.0f / 60.0f),
                configured_62_during_60_fallback, 62);
        if (!Expect(configured_45_scene_fallback.repaired_timing &&
                        configured_62_scene_fallback.repaired_timing,
                    "nearby fixed targets should repair legacy fallback scene "
                    "steps")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker new_explicit_fallback{};
        const float seeded_explicit = spatch::hooks::TrackCutsceneBaseDelta(
            new_explicit_fallback, 1.0f / 30.0f, 144);
        if (!Expect(
                std::fabs(seeded_explicit - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(new_explicit_fallback.stable_delta -
                              (1.0f / 144.0f)) < 0.00001f &&
                    new_explicit_fallback.fallback_candidate_count == 1,
                "a reset explicit timeline should seed from its target, not a "
                "30 Hz fallback")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker cadence_tracker{};
        spatch::hooks::TrackCutsceneBaseDelta(cadence_tracker, 1.0f / 144.0f,
                                              0);
        const float first_30 = spatch::hooks::TrackCutsceneBaseDelta(
            cadence_tracker, 1.0f / 30.0f, 0);
        const float frameflow_view =
            spatch::hooks::ResolveTrackedCutsceneBaseDelta(
                1.0f / 30.0f, cadence_tracker.stable_delta, 0);
        const float nis_view = spatch::hooks::ResolveTrackedCutsceneBaseDelta(
            1.0f / 30.0f, cadence_tracker.stable_delta, 0);
        if (!Expect(
                cadence_tracker.fallback_candidate_count == 1 &&
                    std::fabs(first_30 - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(frameflow_view - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(nis_view - (1.0f / 144.0f)) < 0.00001f,
                "one owner fallback sample should remain one observation "
                "across frameflow and NIS")) {
            return EXIT_FAILURE;
        }
        const float second_30 = spatch::hooks::TrackCutsceneBaseDelta(
            cadence_tracker, 1.0f / 30.0f, 0);
        const float third_30 = spatch::hooks::TrackCutsceneBaseDelta(
            cadence_tracker, 1.0f / 30.0f, 0);
        if (!Expect(
                std::fabs(second_30 - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(third_30 - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(cadence_tracker.stable_delta - (1.0f / 144.0f)) <
                        0.00001f &&
                    cadence_tracker.fallback_candidate_elapsed < 0.5f,
                "brief 30 Hz samples should remain repaired during fallback "
                "hysteresis")) {
            return EXIT_FAILURE;
        }
        float adopted_30 = third_30;
        for (int i = 0; i < 13; ++i) {
            adopted_30 = spatch::hooks::TrackCutsceneBaseDelta(cadence_tracker,
                                                               1.0f / 30.0f, 0);
        }
        if (!Expect(std::fabs(adopted_30 - (1.0f / 30.0f)) < 0.00001f &&
                        std::fabs(cadence_tracker.stable_delta -
                                  (1.0f / 30.0f)) < 0.00001f &&
                        cadence_tracker.fallback_candidate_count == 0,
                    "a 30 Hz cadence should be adopted only after sustained "
                    "evidence")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker sustained_60{};
        spatch::hooks::TrackCutsceneBaseDelta(sustained_60, 1.0f / 144.0f, 0);
        float adopted_60 = 0.0f;
        for (int i = 0; i < 31; ++i) {
            adopted_60 = spatch::hooks::TrackCutsceneBaseDelta(sustained_60,
                                                               1.0f / 60.0f, 0);
        }
        if (!Expect(std::fabs(adopted_60 - (1.0f / 60.0f)) < 0.00001f &&
                        std::fabs(sustained_60.stable_delta - (1.0f / 60.0f)) <
                            0.00001f,
                    "a sustained 60 Hz transition should replace the former "
                    "high-refresh cadence")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker fresh_auto_30{};
        spatch::hooks::TrackCutsceneBaseDelta(fresh_auto_30, 1.0f / 30.0f, 0);
        const float fresh_auto_fast = spatch::hooks::TrackCutsceneBaseDelta(
            fresh_auto_30, 1.0f / 60.0f, 0);
        const float fresh_auto_back = spatch::hooks::TrackCutsceneBaseDelta(
            fresh_auto_30, 1.0f / 30.0f, 0);
        if (!Expect(
                std::fabs(fresh_auto_fast - (1.0f / 60.0f)) < 0.00001f &&
                    std::fabs(fresh_auto_back - (1.0f / 30.0f)) < 0.00001f &&
                    std::fabs(fresh_auto_30.stable_delta - (1.0f / 30.0f)) <
                        0.00001f &&
                    fresh_auto_30.fallback_candidate_count == 0,
                "a fresh Auto 30 Hz cadence should survive one faster sample "
                "and bounce back")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker fresh_auto_60{};
        spatch::hooks::TrackCutsceneBaseDelta(fresh_auto_60, 1.0f / 60.0f, 0);
        const float fresh_auto_144 = spatch::hooks::TrackCutsceneBaseDelta(
            fresh_auto_60, 1.0f / 144.0f, 0);
        const float fresh_auto_60_back = spatch::hooks::TrackCutsceneBaseDelta(
            fresh_auto_60, 1.0f / 60.0f, 0);
        if (!Expect(
                std::fabs(fresh_auto_144 - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(fresh_auto_60_back - (1.0f / 60.0f)) < 0.00001f &&
                    std::fabs(fresh_auto_60.stable_delta - (1.0f / 60.0f)) <
                        0.00001f &&
                    fresh_auto_60.fallback_candidate_count == 0,
                "a fresh Auto 60 Hz cadence should survive one 144 Hz sample "
                "and bounce back")) {
            return EXIT_FAILURE;
        }

        const float adopted_auto_fast = spatch::hooks::TrackCutsceneBaseDelta(
            cadence_tracker, 1.0f / 60.0f, 0);
        const float adopted_auto_back = spatch::hooks::TrackCutsceneBaseDelta(
            cadence_tracker, 1.0f / 30.0f, 0);
        if (!Expect(
                std::fabs(adopted_auto_fast - (1.0f / 60.0f)) < 0.00001f &&
                    std::fabs(adopted_auto_back - (1.0f / 30.0f)) < 0.00001f &&
                    std::fabs(cadence_tracker.stable_delta - (1.0f / 30.0f)) <
                        0.00001f &&
                    cadence_tracker.fallback_candidate_count == 0,
                "an adopted Auto 30 Hz cadence should reject a one-frame "
                "faster bounce")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker long_frame_tracker{};
        spatch::hooks::TrackCutsceneBaseDelta(long_frame_tracker, 1.0f / 144.0f,
                                              0);
        const float live_40ms = spatch::hooks::TrackCutsceneBaseDelta(
            long_frame_tracker, 0.040f, 0);
        const float after_40ms_zero =
            spatch::hooks::TrackCutsceneBaseDelta(long_frame_tracker, 0.0f, 0);
        if (!Expect(
                std::fabs(live_40ms - 0.040f) < 0.00001f &&
                    std::fabs(after_40ms_zero - (1.0f / 144.0f)) < 0.00001f,
                "one real long frame should be preserved without poisoning the "
                "next missing tick")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::ResetCutsceneCadenceTracker(long_frame_tracker);
        spatch::hooks::TrackCutsceneBaseDelta(long_frame_tracker, 1.0f / 144.0f,
                                              0);
        const float new_instance_zero =
            spatch::hooks::TrackCutsceneBaseDelta(long_frame_tracker, 0.0f, 0);
        if (!Expect(
                std::fabs(new_instance_zero - (1.0f / 144.0f)) < 0.00001f,
                "a new active instance should retain its first sane cadence "
                "for the next zero frame")) {
            return EXIT_FAILURE;
        }

        if (!Expect(std::fabs(spatch::hooks::SelectCutsceneCorrectionDelta(
                                  0.0014f, 1000) -
                              0.0014f) < 0.000001f &&
                        std::fabs(spatch::hooks::SelectCutsceneCorrectionDelta(
                                      0.0024f, 500) -
                                  0.0024f) < 0.000001f,
                    "high-rate cadence matching should retain its documented "
                    "five-percent tolerance")) {
            return EXIT_FAILURE;
        }

        const auto normal_hitch = ResolveCutsceneFrameflowDelta(
            true, CutscenePauseState{}, true, 0, 0.020f, 1.0f / 120.0f);
        if (!Expect(
                !normal_hitch.applied_zero_dt_fix &&
                    std::fabs(normal_hitch.forwarded_dt - 0.020f) < 0.000001f,
                "ordinary non-zero hitches should not be quantized by "
                "frameflow")) {
            return EXIT_FAILURE;
        }

        const auto scene_30hz_anomaly = spatch::hooks::ResolveCutsceneSceneTime(
            true, false, true, false, 1.0f, 1.0f + (1.0f / 30.0f),
            1.0f / 120.0f);
        if (!Expect(scene_30hz_anomaly.corrected &&
                        scene_30hz_anomaly.applied_scene_time > 1.008f &&
                        scene_30hz_anomaly.applied_scene_time < 1.009f,
                    "state-aware NIS timing should repair an observed 30 Hz "
                    "anomaly")) {
            return EXIT_FAILURE;
        }

        const auto configured_scene_30hz_fallback =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 1.0f, 1.0f, 1.0f + (1.0f / 30.0f),
                1.0f / 144.0f, 144);
        if (!Expect(
                configured_scene_30hz_fallback.repaired_timing &&
                    configured_scene_30hz_fallback.applied_scene_time >
                        1.006f &&
                    configured_scene_30hz_fallback.applied_scene_time < 1.008f,
                "an explicit 144 Hz target should repair an initial legacy 30 "
                "Hz fallback")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker explicit_slowdown{};
        spatch::hooks::TrackCutsceneBaseDelta(explicit_slowdown, 1.0f / 144.0f,
                                              144);
        float previous_raw = 5.0f;
        float previous_applied = 5.0f;
        std::array<spatch::hooks::CutsceneSceneTimeDecision, 20>
            sustained_decisions{};
        for (std::size_t i = 0; i < sustained_decisions.size(); ++i) {
            const float tracked = spatch::hooks::TrackCutsceneBaseDelta(
                explicit_slowdown, 1.0f / 30.0f, 144);
            const float raw = previous_raw + (1.0f / 30.0f);
            sustained_decisions[i] = spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, previous_raw, previous_applied, raw,
                tracked, 144,
                spatch::hooks::IsLegacyFallbackPending(explicit_slowdown, 144));
            previous_raw = raw;
            previous_applied = sustained_decisions[i].applied_scene_time;
        }
        const float early_explicit_delta =
            sustained_decisions[2].applied_scene_time -
            sustained_decisions[1].applied_scene_time;
        const float adopted_explicit_delta =
            sustained_decisions.back().applied_scene_time -
            sustained_decisions[sustained_decisions.size() - 2]
                .applied_scene_time;
        if (!Expect(
                sustained_decisions[0].repaired_timing &&
                    sustained_decisions[7].repaired_timing &&
                    !sustained_decisions.back().repaired_timing &&
                    early_explicit_delta < 0.034f &&
                    std::fabs(adopted_explicit_delta - (1.0f / 30.0f)) <
                        0.00001f,
                "explicit mode should repair brief 30 Hz leaks then adopt only "
                "a sustained slowdown")) {
            return EXIT_FAILURE;
        }

        spatch::hooks::CutsceneCadenceTracker explicit_60{};
        spatch::hooks::TrackCutsceneBaseDelta(explicit_60, 1.0f / 144.0f, 144);
        float explicit_60_last = 0.0f;
        for (int i = 0; i < 31; ++i) {
            explicit_60_last = spatch::hooks::TrackCutsceneBaseDelta(
                explicit_60, 1.0f / 60.0f, 144);
        }
        if (!Expect(
                std::fabs(explicit_60_last - (1.0f / 60.0f)) < 0.00001f &&
                    std::fabs(explicit_60.stable_delta - (1.0f / 60.0f)) <
                        0.00001f,
                "explicit mode should adopt a sustained 60 Hz slowdown after "
                "duration hysteresis")) {
            return EXIT_FAILURE;
        }

        // Integer targets adjacent to the stock 30/60 rates must still be
        // distinguishable from those fallbacks.  Exercise both the pending
        // repair decision and the post-adoption state, including the
        // previously problematic 62-vs-60 snap boundary.
        for (const int target : {31, 32, 45, 61, 62, 144}) {
            spatch::hooks::CutsceneCadenceTracker boundary_tracker{};
            spatch::hooks::TrackCutsceneBaseDelta(boundary_tracker,
                                                  1.0f / 144.0f, target);
            const float legacy_delta =
                target > 60 ? (1.0f / 60.0f) : (1.0f / 30.0f);
            const float pending_dt = spatch::hooks::TrackCutsceneBaseDelta(
                boundary_tracker, legacy_delta, target);
            const auto pending_decision =
                spatch::hooks::ResolveCutsceneSceneTime(
                    true, false, true, false, 10.0f, 10.0f,
                    10.0f + legacy_delta, pending_dt, target,
                    spatch::hooks::IsLegacyFallbackPending(boundary_tracker,
                                                           target));
            if (!Expect(
                    pending_decision.repaired_timing,
                    "configured rates near a legacy cadence should repair the "
                    "pending sample")) {
                return EXIT_FAILURE;
            }
        }

        spatch::hooks::CutsceneCadenceTracker target_62_tracker{};
        spatch::hooks::TrackCutsceneBaseDelta(target_62_tracker, 1.0f / 144.0f,
                                              62);
        for (int i = 0; i < 31; ++i) {
            spatch::hooks::TrackCutsceneBaseDelta(target_62_tracker,
                                                  1.0f / 60.0f, 62);
        }
        if (!Expect(std::fabs(target_62_tracker.stable_delta - (1.0f / 60.0f)) <
                            0.00001f &&
                        !spatch::hooks::IsLegacyFallbackPending(
                            target_62_tracker, 62),
                    "a sustained 60 Hz sample must be adopted as 60, not "
                    "snapped back "
                    "to 62")) {
            return EXIT_FAILURE;
        }
        const auto adopted_62_scene = spatch::hooks::ResolveCutsceneSceneTime(
            true, false, true, false, 20.0f, 20.0f, 20.0f + (1.0f / 60.0f),
            target_62_tracker.stable_delta, 62,
            spatch::hooks::IsLegacyFallbackPending(target_62_tracker, 62));
        if (!Expect(
                !adopted_62_scene.repaired_timing,
                "an adopted 60 Hz cadence should stop accumulating stale 62 Hz "
                "repairs")) {
            return EXIT_FAILURE;
        }
        const float recovered_62 = spatch::hooks::TrackCutsceneBaseDelta(
            target_62_tracker, 1.0f / 144.0f, 62);
        const float recovered_62_zero =
            spatch::hooks::TrackCutsceneBaseDelta(target_62_tracker, 0.0f, 62);
        if (!Expect(
                std::fabs(recovered_62 - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(recovered_62_zero - (1.0f / 144.0f)) < 0.00001f &&
                    std::fabs(target_62_tracker.stable_delta -
                              (1.0f / 144.0f)) < 0.00001f,
                "a high-rate recovery should clear the adopted legacy cadence "
                "before a zero sample")) {
            return EXIT_FAILURE;
        }

        const auto scene_observed_short_step =
            spatch::hooks::ResolveCutsceneSceneTime(true, false, true, false,
                                                    2.0f, 2.022f, 1.0f / 60.0f);
        if (!Expect(
                scene_observed_short_step.corrected &&
                    scene_observed_short_step.applied_scene_time > 2.016f &&
                    scene_observed_short_step.applied_scene_time < 2.018f,
                "state-aware NIS timing should retain the observed 21-23 ms "
                "recovery")) {
            return EXIT_FAILURE;
        }

        const auto scene_observed_doubled_step =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 3.0f, 3.043f, 1.0f / 240.0f);
        if (!Expect(
                scene_observed_doubled_step.corrected &&
                    scene_observed_doubled_step.applied_scene_time > 3.004f &&
                    scene_observed_doubled_step.applied_scene_time < 3.005f,
                "high-refresh NIS timing should repair the observed doubled "
                "step")) {
            return EXIT_FAILURE;
        }

        const auto scene_hitch = spatch::hooks::ResolveCutsceneSceneTime(
            true, false, true, false, 1.0f, 1.0f + 0.020f, 1.0f / 120.0f);
        if (!Expect(!scene_hitch.corrected &&
                        scene_hitch.applied_scene_time > 1.019f,
                    "ordinary scene-time hitches should remain measurable")) {
            return EXIT_FAILURE;
        }

        const auto matched_long_frame = spatch::hooks::ResolveCutsceneSceneTime(
            true, false, true, false, 1.0f, 1.040f, 0.040f);
        if (!Expect(
                !matched_long_frame.corrected &&
                    matched_long_frame.applied_scene_time > 1.039f,
                "a real long frame should be preserved when scene time matches "
                "live time")) {
            return EXIT_FAILURE;
        }

        const auto configured_matched_long_frame =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 1.0f, 1.0f, 1.040f, 0.040f, 120);
        if (!Expect(
                !configured_matched_long_frame.repaired_timing &&
                    configured_matched_long_frame.applied_scene_time > 1.039f,
                "an explicit target should preserve a genuine matched long "
                "frame")) {
            return EXIT_FAILURE;
        }

        const auto low_rate_43ms_frame =
            spatch::hooks::ResolveCutsceneSceneTime(true, false, true, false,
                                                    1.0f, 1.043f, 1.0f / 30.0f);
        if (!Expect(
                !low_rate_43ms_frame.repaired_timing &&
                    low_rate_43ms_frame.applied_scene_time > 1.042f,
                "the captured 43 ms high-refresh anomaly band should not alter "
                "30 FPS operation")) {
            return EXIT_FAILURE;
        }

        const float tick_240 = 1.0f / 240.0f;
        const auto repeated_raw_tick_1 =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 0.0f, 0.0f, 0.0f, tick_240, 0);
        const auto repeated_raw_tick_2 =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, false, 0.0f,
                repeated_raw_tick_1.applied_scene_time, 0.0f, tick_240, 0);
        const auto resumed_raw_tick = spatch::hooks::ResolveCutsceneSceneTime(
            true, false, true, false, 0.0f,
            repeated_raw_tick_2.applied_scene_time, tick_240, tick_240, 0);
        if (!Expect(
                repeated_raw_tick_1.repaired_timing &&
                    repeated_raw_tick_2.applied_scene_time >
                        repeated_raw_tick_1.applied_scene_time &&
                    resumed_raw_tick.applied_scene_time >
                        repeated_raw_tick_2.applied_scene_time,
                "separate raw/applied history should remain monotonic across "
                "repeated high-FPS timestamps")) {
            return EXIT_FAILURE;
        }

        const auto sync_resets_history =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, true, true, 0.0f,
                repeated_raw_tick_2.applied_scene_time, 0.001f, tick_240, 0);
        const auto new_instance_bypasses_history =
            spatch::hooks::ResolveCutsceneSceneTime(
                true, false, false, false, 0.0f,
                repeated_raw_tick_2.applied_scene_time, 0.001f, tick_240, 0);
        if (!Expect(
                !sync_resets_history.corrected &&
                    std::fabs(sync_resets_history.applied_scene_time - 0.001f) <
                        0.000001f &&
                    !new_instance_bypasses_history.corrected &&
                    std::fabs(new_instance_bypasses_history.applied_scene_time -
                              0.001f) < 0.000001f,
                "sync and active-instance transitions should reset accumulated "
                "scene-time history")) {
            return EXIT_FAILURE;
        }

        const auto paused_scene = spatch::hooks::ResolveCutsceneSceneTime(
            true, true, true, false, 1.0f, 1.0f, 1.0f / 120.0f);
        if (!Expect(
                !paused_scene.corrected &&
                    paused_scene.applied_scene_time == 1.0f,
                "paused NIS scene time should not be advanced by the fix")) {
            return EXIT_FAILURE;
        }

        const auto sync_scene = spatch::hooks::ResolveCutsceneSceneTime(
            true, false, true, true, 1.0f, 1.0f, 1.0f / 120.0f);
        if (!Expect(
                !sync_scene.corrected && sync_scene.applied_scene_time == 1.0f,
                "explicit NIS synchronization should bypass correction")) {
            return EXIT_FAILURE;
        }

        if (!Expect(!spatch::hooks::CanRetainCutsceneTimelineHistory(true, 5.0f,
                                                                     4.5f),
                    "an unflagged backward scene-time jump should reset timing "
                    "history")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                spatch::hooks::CanRetainCutsceneTimelineHistory(true, 5.0f,
                                                                5.0f),
                "equal applied scene time should remain a valid timeline")) {
            return EXIT_FAILURE;
        }
        if (!Expect(spatch::hooks::CanRetainCutsceneTimelineHistory(false, 5.0f,
                                                                    1.0f),
                    "a reset call should become the valid baseline of its new "
                    "timeline")) {
            return EXIT_FAILURE;
        }
    }

    {
        using spatch::hooks::nisprobe::ResolveRestoreForwardingDecision;
        using spatch::hooks::nisprobe::RestoreDisposition;

        const auto tracked =
            ResolveRestoreForwardingDecision(RestoreDisposition::tracked, true);
        if (!Expect(tracked.call_original,
                    "tracked NIS actor restores should "
                    "still call the original restore")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !tracked.suppressed_duplicate,
                "tracked NIS actor restores should not be marked as duplicate "
                "suppressions")) {
            return EXIT_FAILURE;
        }

        const auto duplicate_passthrough = ResolveRestoreForwardingDecision(
            RestoreDisposition::duplicate, false);
        if (!Expect(
                duplicate_passthrough.call_original,
                "duplicate NIS actor restores should pass through when the fix "
                "is disabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!duplicate_passthrough.suppressed_duplicate,
                    "disabled duplicate-restore fix should not mark a "
                    "suppression")) {
            return EXIT_FAILURE;
        }

        const auto duplicate_suppressed = ResolveRestoreForwardingDecision(
            RestoreDisposition::duplicate, true);
        if (!Expect(
                !duplicate_suppressed.call_original,
                "duplicate NIS actor restores should be suppressed when the "
                "fix is enabled")) {
            return EXIT_FAILURE;
        }
        if (!Expect(duplicate_suppressed.suppressed_duplicate,
                    "enabled duplicate-restore fix should report the "
                    "suppression")) {
            return EXIT_FAILURE;
        }

        const auto never_seen = ResolveRestoreForwardingDecision(
            RestoreDisposition::never_seen, true);
        if (!Expect(
                never_seen.call_original,
                "never-seen NIS actor restores should still call the original "
                "restore")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!never_seen.suppressed_duplicate,
                    "never-seen NIS actor restores should not be marked as "
                    "duplicate suppressions")) {
            return EXIT_FAILURE;
        }
    }

    {
        if (!Expect(
                ClampFogSlicingInterval(-1, 1) == 1 &&
                    ClampFogSlicingInterval(0, 3) == 3 &&
                    ClampFogSlicingInterval(2, 1) == 2 &&
                    ClampFogSlicingInterval(5, 1) == 4 &&
                    ClampFogSlicingInterval(
                        (std::numeric_limits<int>::max)(),
                        (std::numeric_limits<int>::max)()) == 4,
                "fog slicing policy should keep every engine divisor in the "
                "supported 1-4 range")) {
            return EXIT_FAILURE;
        }
    }

    {
        using namespace spatch::fog_restoration;
        if (!Expect(
                kOriginalGameIntensity == 0.0f &&
                    SetterRva(false) == kLegacySetterRva &&
                    SetterRva(true) == kLatestSteamSetterRva &&
                    SignatureTargetsIntensity(kLegacySetterRva,
                                              kLegacySetterSignature) &&
                    SignatureTargetsIntensity(kLatestSteamSetterRva,
                                              kLatestSteamSetterSignature),
                "original fog policy should map both verified DE setters to "
                "the shared intensity")) {
            return EXIT_FAILURE;
        }
        auto damaged_signature = kLegacySetterSignature;
        damaged_signature[16] = 0x90;
        if (!Expect(
                !SignatureTargetsIntensity(kLegacySetterRva, damaged_signature),
                "original fog policy should reject a changed setter opcode")) {
            return EXIT_FAILURE;
        }
    }

    {
        const auto path = MakeTempIniPath(L"spatch-config-clamp-test.ini");
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=9\n"
               << "min_fog_slicing_interval=0\n"
               << "disable_time_step_smoothing=1\n"
               << "time_step_smoothing_frames=9\n"
               << "override_world_density=9\n"
               << "override_fullscreen=9\n"
               << "smaa_preset=9\n"
               << "spherical_reflection_width=99999\n"
               << "max_verbose_events=999999999\n"
               << "max_unique_callbacks=999999999\n"
               << "summary_interval_ms=999999999\n"
               << "[Graphics]\n"
               << "WetnessFullTime=-50\n"
               << "WetnessFadeTime=999999\n"
               << "SweatBuildTime=-2\n"
               << "SweatFadeTime=999999\n"
               << "SweatOnsetTime=999999\n"
               << "SweatRunSpeed=9999\n"
               << "SweatCombatTime=-4\n";
        stream.close();

        const Config config = LoadConfig(path);
        if (!Expect(config.min_fog_slicing_interval == 1,
                    "fog slicing interval should clamp to 1")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                config.time_step_smoothing == 0,
                "legacy smoothing-disable controls should migrate to Off")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.override_world_density == 4,
                    "override_world_density should clamp to 4")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.override_fullscreen == 1,
                    "override_fullscreen should clamp to 1")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.smaa_preset == 3, "smaa_preset should clamp to 3")) {
            return EXIT_FAILURE;
        }
        if (!Expect(config.spherical_reflection_width == 4096,
                    "spherical reflection width should clamp to 4096")) {
            return EXIT_FAILURE;
        }
#if defined(SPATCH_FINAL_RELEASE)
        const bool diagnostic_limits_match_policy =
            config.max_verbose_events == 0 &&
            config.max_unique_callbacks == 0 &&
            config.summary_interval_ms == 0;
#else
        const bool diagnostic_limits_match_policy =
            config.max_verbose_events == 100000 &&
            config.max_unique_callbacks == 100000 &&
            config.summary_interval_ms == 3600000;
#endif
        if (!Expect(diagnostic_limits_match_policy,
                    "diagnostic limits should be bounded or compiled out by "
                    "final-release policy")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                config.wetness_full_time_seconds == kWetnessTimeMinSeconds &&
                    config.wetness_fade_time_seconds == kWetnessTimeMaxSeconds,
                "wetness durations should clamp to their documented range")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                config.sweat_build_time_seconds == kSweatTimeMinSeconds &&
                    config.sweat_fade_time_seconds == kSweatTimeMaxSeconds &&
                    config.sweat_onset_time_seconds == kSweatTimeMaxSeconds &&
                    config.sweat_run_speed == kSweatRunSpeedMax &&
                    config.sweat_combat_time_seconds == kSweatTimeMinSeconds,
                "sweat controls should clamp to their documented range")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-wetness-invalid-config-test.ini");
        RemoveIfExists(path);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=9\n"
               << "[Graphics]\n"
               << "WetnessFullTime=thirty\n"
               << "WetnessFadeTime=\n"
               << "SweatBuildTime=slow\n"
               << "SweatFadeTime=\n"
               << "SweatOnsetTime=late\n"
               << "SweatRunSpeed=fast\n"
               << "SweatCombatTime=\n";
        stream.close();

        const Config config = LoadConfig(path);
        if (!Expect(
                config.wetness_full_time_seconds == 30 &&
                    config.wetness_fade_time_seconds == 270 &&
                    config.sweat_build_time_seconds == 150 &&
                    config.sweat_fade_time_seconds == 120 &&
                    config.sweat_onset_time_seconds == 30 &&
                    std::fabs(config.sweat_run_speed - 2.5f) < 0.0001f &&
                    config.sweat_combat_time_seconds == 15,
                "malformed wetness and sweat controls should use documented "
                "defaults")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-config-fog-upper-clamp-test.ini");
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[Stability]\n"
               << "MinimumFogSlicingInterval=2147483647\n";
        stream.close();
        const Config config = LoadConfig(path);
        if (!Expect(config.min_fog_slicing_interval == 4,
                    "fog slicing config should clamp its upper bound to 4")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path = MakeTempIniPath(L"spatch-config-grouped-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "allow_unsupported_build=1\n"
               << "vram_override_mb=4096\n"
               << "hook_queue_ready=1\n"
               << "aa_variant_mode=2\n"
               << "[Cutscenes]\n"
               << "cutscene_fps=144\n"
               << "fix_cutscene_zero_dt=1\n"
               << "min_fog_slicing_interval=3\n"
               << "[Display]\n"
               << "warn_low_res_buffer=0\n"
               << "[Diagnostics]\n"
               << "summary_interval_ms=0\n";
        stream.close();
        {
            std::ofstream stale_backup(backup, std::ios::out | std::ios::trunc);
            stale_backup << "stale-backup\n";
        }

        const Config grouped = LoadConfig(path);
        if (!Expect(grouped.cutscene_fps == 144,
                    "grouped Cutscenes section should parse an arbitrary "
                    "target FPS")) {
            return EXIT_FAILURE;
        }
        if (!Expect(grouped.allow_unverified_build &&
                        grouped.min_fog_slicing_interval == 3 &&
                        !grouped.warn_low_res_buffer,
                    "migration should preserve supported public behavior")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!grouped.hook_queue_ready && grouped.aa_variant_mode == 0,
                    "config migration should explicitly retire legacy "
                    "developer-only controls")) {
            return EXIT_FAILURE;
        }
        if (!Expect(grouped.summary_interval_ms == 0,
                    "grouped Diagnostics section should preserve disabled "
                    "summaries")) {
            return EXIT_FAILURE;
        }

        std::ifstream migrated(path);
        std::string migrated_text((std::istreambuf_iterator<char>(migrated)),
                                  {});
        if (!Expect(
                migrated_text.find("[SPatch]") != std::string::npos &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("AllowUnverifiedBuild=1") !=
                        std::string::npos &&
                    migrated_text.find("MinimumFogSlicingInterval=3") !=
                        std::string::npos &&
                    migrated_text.find("WarnLowResolutionBuffer=0") !=
                        std::string::npos &&
                    migrated_text.find("VRAMOverrideMB") == std::string::npos &&
                    migrated_text.find("[Debug]") != std::string::npos &&
                    migrated_text.find("Logging=0") != std::string::npos &&
                    migrated_text.find("[Diagnostics]") == std::string::npos &&
                    migrated_text.find("EnableLogging=") == std::string::npos &&
                    migrated_text.find("SummaryInterval=") == std::string::npos &&
                    migrated_text.find("hook_queue_ready") ==
                        std::string::npos &&
                    migrated_text.find("aa_variant_mode") == std::string::npos,
                "legacy-version config should be rewritten with the organized "
                "header")) {
            return EXIT_FAILURE;
        }
        if (!Expect(std::filesystem::exists(backup),
                    "config migration should preserve the original ini")) {
            return EXIT_FAILURE;
        }
        std::ifstream backup_stream(backup);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                backup_text.find("cutscene_fps=144") != std::string::npos &&
                    backup_text.find("stale-backup") == std::string::npos,
                "migration should replace a stale backup with the exact file "
                "being rewritten")) {
            return EXIT_FAILURE;
        }

        const Config grouped_second_load = LoadConfig(path);
        if (!Expect(
                grouped_second_load.allow_unverified_build &&
                    grouped_second_load.min_fog_slicing_interval == 3 &&
                    !grouped_second_load.warn_low_res_buffer &&
                    !grouped_second_load.hook_queue_ready &&
                    grouped_second_load.aa_variant_mode == 0,
                "a migrated config should preserve its behavior on a second "
                "load")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v5-section-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=5\n"
               << "CutsceneFPS=165\n"
               << "FixCutsceneFPS=0\n"
               << "DisableTimeStepSmoothing=0\n"
               << "TimeStepSmoothingFrames=-1\n"
               << "FixFogSlicing=0\n"
               << "RestoreVisualDamage=0\n"
               << "RestoreCharacterShadows=0\n"
               << "FixPedestrianDensity=0\n"
               << "FixHighFPSAverages=0\n"
               << "ImproveSphericalReflections=0\n"
               << "SphericalReflectionWidth=1920\n"
               << "UseMaximumRefreshRate=1\n"
               << "Remove120FPSCap=0\n"
               << "Fullscreen=0\n"
               << "FrameLimiter=3\n"
               << "LowResolutionBuffer=-1\n"
               << "ShadowFilter=0\n"
               << "TextureDetail=1\n"
               << "WorldDensity=2\n"
               << "Rumble=0\n"
               << "SMAA=0\n"
               << "DisableStockAA=0\n"
               << "SMAAPreset=3\n";
        stream.close();

        const auto expect_moved_values = [](const Config& config) {
            return config.cutscene_fps == 165 && !config.fix_cutscene_zero_dt &&
                   !config.fix_cutscene_scene_time_step &&
                   config.time_step_smoothing == -1 &&
                   !config.hook_fog_slicing_guard &&
                   !config.fix_pedestrian_density_at_high_fps &&
                   !config.fix_high_fps_average_windows &&
                   !config.improve_spherical_reflections &&
                   config.spherical_reflection_width == 1920 &&
                   config.prefer_max_refresh_rate &&
                   !config.remove_hidden_120_fps_cap &&
                   config.override_fullscreen == 0 &&
                   config.override_fps_limiter == 3 &&
                   config.override_low_res_buffer == -1 &&
                   config.override_shadow_filter == 0 &&
                   config.override_texture_detail_level == 1 &&
                   config.override_world_density == 2 &&
                   config.override_rumble_enabled == 0 &&
                    !config.hook_smaa_present && !config.smaa_enable &&
                    !config.smaa_disable_stock_aa && config.smaa_preset == 3;
        };
        const Config migrated = LoadConfig(path);
        if (!Expect(
                expect_moved_values(migrated),
                "v5 canonical keys under SPatch should survive the organized "
                "section migration")) {
            return EXIT_FAILURE;
        }
        if (!Expect(expect_moved_values(LoadConfig(path)),
                    "moved settings should retain identical behavior after "
                    "migration reload")) {
            return EXIT_FAILURE;
        }
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                migrated_text.find("RestoreVisualDamage=") ==
                        std::string::npos &&
                    backup_text.find("RestoreVisualDamage=0") !=
                        std::string::npos,
                "v5 migration should retire visual-damage restoration while "
                "preserving the original setting in the exact backup")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-legacy-group-routing-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=6\n"
               << "[Graphics]\n"
               << "restore_character_visual_damage=0\n"
               << "[Display]\n"
               << "warn_low_res_buffer=0\n";
        stream.close();

        const Config routed = LoadConfig(path);
        std::ifstream migrated(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated)), {});
        if (!Expect(
                routed.restore_character_wetness &&
                    routed.restore_character_sweat &&
                    !routed.warn_low_res_buffer &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("RestoreWetness=1") !=
                        std::string::npos &&
                    migrated_text.find("RestoreSweat=1") != std::string::npos &&
                    migrated_text.find("restore_character_visual_damage=") ==
                        std::string::npos,
                "v6 migration should add supported surface restorations and "
                "retire visual-damage restoration")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v8-sweat-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=8\n"
               << "[Graphics]\n"
               << "RestoreWetness=0\n"
               << "WetnessFullTime=45\n"
               << "WetnessFadeTime=600\n"
               << "RestoreVisualDamage=0\n";
        stream.close();

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                !migrated.restore_character_wetness &&
                    migrated.restore_character_sweat &&
                    migrated.wetness_full_time_seconds == 45 &&
                    migrated.wetness_fade_time_seconds == 600 &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("RestoreSweat=1") != std::string::npos &&
                    migrated_text.find("WetnessFullTime=45") !=
                        std::string::npos &&
                    migrated_text.find("WetnessFadeTime=600") !=
                        std::string::npos &&
                    migrated_text.find("RestoreVisualDamage=") ==
                        std::string::npos &&
                    backup_text.find("ConfigVersion=8") != std::string::npos &&
                    backup_text.find("RestoreVisualDamage=0") !=
                        std::string::npos,
                "v8 migration should preserve choices and add sweat "
                "restoration")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path = MakeTempIniPath(
            L"spatch-v42-retired-visual-damage-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=44\n"
            "restore_character_visual_damage=1\n"
            "[Graphics]\n"
            "RestoreVisualDamage=0\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        (void)LoadConfig(path, &report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        ConfigLoadReport reload_report{};
        (void)LoadConfig(path, &reload_report);
        if (!Expect(
                report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    reload_report.source_version == kConfigVersion &&
                    reload_report.persistence ==
                        ConfigPersistenceStatus::Unchanged &&
                    migrated_text.find("RestoreVisualDamage=") ==
                        std::string::npos &&
                    migrated_text.find("restore_character_visual_damage=") ==
                        std::string::npos &&
                    backup_text == source_text,
                "current visual-damage settings should be removed once with "
                "an exact backup")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v42-logging-precedence-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        const std::string source_text =
            "[SPatch]\n"
            "ConfigVersion=44\n"
            "enable_logging=1\n"
            "[Debug]\n"
            "Logging=0\n"
            "[Diagnostics]\n"
            "EnableLogging=1\n";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << source_text;
        }

        ConfigLoadReport report{};
        const Config migrated = LoadConfig(path, &report);
        std::ifstream migrated_stream(path, std::ios::binary);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup, std::ios::binary);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                !migrated.enable_logging &&
                    report.source_version == kConfigVersion &&
                    report.persistence == ConfigPersistenceStatus::Migrated &&
                    migrated_text.find("[Debug]") != std::string::npos &&
                    migrated_text.find("Logging=0") != std::string::npos &&
                    migrated_text.find("[Diagnostics]") == std::string::npos &&
                    migrated_text.find("EnableLogging=") == std::string::npos &&
                    migrated_text.find("enable_logging=") == std::string::npos &&
                    backup_text == source_text,
                "canonical Debug Logging should win over legacy aliases and "
                "rewrite them once with an exact backup")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path = MakeTempIniPath(
            L"spatch-v12-original-atmosphere-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=12\n"
               << "[Graphics]\n"
               << "RestoreOriginalFog=0\n"
               << "RestoreWetness=0\n";
        stream.close();

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                !migrated.restore_original_fog &&
                    !migrated.restore_character_wetness &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("RestoreOriginalFogAndNeon=0") !=
                        std::string::npos &&
                    migrated_text.find("RestoreOriginalAtmosphere=") ==
                        std::string::npos &&
                    migrated_text.find("RestoreOriginalEyeReflections=1") !=
                        std::string::npos &&
                    migrated_text.find("RestoreOriginalFog=") ==
                        std::string::npos &&
                    backup_text.find("ConfigVersion=12") != std::string::npos &&
                    backup_text.find("RestoreOriginalFog=0") !=
                        std::string::npos,
                "v12 migration should rename and preserve the fog and neon "
                "choice")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-v14-fog-and-neon-migration-test.ini");
        const auto backup = ConfigBackupPath(path);
        RemoveIfExists(path);
        RemoveIfExists(backup);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=14\n"
               << "[Graphics]\n"
               << "RestoreOriginalAtmosphere=0\n";
        stream.close();

        const Config migrated = LoadConfig(path);
        std::ifstream migrated_stream(path);
        const std::string migrated_text(
            (std::istreambuf_iterator<char>(migrated_stream)), {});
        std::ifstream backup_stream(backup);
        const std::string backup_text(
            (std::istreambuf_iterator<char>(backup_stream)), {});
        if (!Expect(
                !migrated.restore_original_fog &&
                    migrated_text.find("ConfigVersion=44") !=
                        std::string::npos &&
                    migrated_text.find("RestoreOriginalFogAndNeon=0") !=
                        std::string::npos &&
                    migrated_text.find("RestoreOriginalAtmosphere=") ==
                        std::string::npos &&
                    backup_text.find("ConfigVersion=14") != std::string::npos &&
                    backup_text.find("RestoreOriginalAtmosphere=0") !=
                        std::string::npos,
                "v14 migration should preserve and rename the atmosphere "
                "choice")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
        RemoveIfExists(backup);
    }


    {
        const auto path = MakeTempIniPath(L"spatch-config-user-names-test.ini");
        RemoveIfExists(path);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "[SPatch]\n"
               << "ConfigVersion=9\n"
               << "[Cutscenes]\n"
               << "CutsceneFPS=165\n"
               << "FixCutsceneFPS=0\n"
               << "[Graphics]\n"
               << "RestoreOriginalFogAndNeon=0\n"
               << "RestoreOriginalAtmosphere=1\n"
               << "RestoreOriginalEyeReflections=0\n"
               << "RestoreOriginalFog=1\n"
               << "restore_original_fog=1\n"
               << "RestoreWetness=0\n"
               << "RestoreSweat=0\n"
               << "restore_character_sweat=1\n"
               << "WetnessFullTime=45\n"
               << "WetnessFadeTime=600\n"
               << "wetness_full_time_seconds=10\n"
               << "wetness_fade_time_seconds=20\n"
               << "FixVRAMReporting=0\n"
               << "[Display]\n"
               << "FixFirstRunResolution=0\n"
               << "[AntiAliasing]\n"
               << "SMAA=0\n"
               << "[Stability]\n"
               << "FixScaleformTimerOverflow=0\n"
               << "FixFileTimestampUpdates=0\n"
               << "FixAudioFileOpen=0\n"
               << "FixLargeFileSizes=0\n"
               << "FixVRAMPoolLock=0\n"
               << "FixResourceLoading=0\n"
               << "FixContactListOverflow=0\n"
               << "FixCorruptSaveCrash=0\n"
               << "FixThreadCreationFailure=0\n"
               << "[Diagnostics]\n"
               << "EnableLogging=1\n"
               << "SummaryInterval=2500\n";
        stream.close();

        const Config user_names = LoadConfig(path);
        if (!Expect(
                user_names.cutscene_fps == 165 &&
                    !user_names.fix_cutscene_zero_dt &&
                    !user_names.fix_cutscene_scene_time_step &&
                    !user_names.hook_nis_timing && !user_names.hook_nis_owner &&
                    !user_names.hook_frameflow,
                "FixCutsceneFPS=0 should disable both timing fixes and their "
                "internal-only detours")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!user_names.hook_smaa_present && !user_names.smaa_enable,
                    "SMAA should control both the runtime and its hook")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !user_names.restore_original_fog &&
                    !user_names.restore_original_eye_reflections &&
                    !user_names.restore_character_wetness &&
                    !user_names.restore_character_sweat &&
                    user_names.wetness_full_time_seconds == 45 &&
                    user_names.wetness_fade_time_seconds == 600,
                "canonical graphics settings should parse and override legacy "
                "duplicates")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!user_names.fix_vram_capacity_reporting &&
                        !user_names.fix_first_run_resolution &&
                        !user_names.fix_scaleform_qpc_clock &&
                        !user_names.fix_file_timestamp_open_mode &&
                        !user_names.fix_audio_file_open &&
                        !user_names.fix_large_file_sizes &&
                        !user_names.fix_vram_pool_lock &&
                        !user_names.fix_resource_loading &&
                        !user_names.fix_contact_list_overflow &&
                        !user_names.fix_corrupt_save_handling &&
                        !user_names.fix_thread_creation_failure,
                    "Stability section should parse end-user engine guard "
                    "switches")) {
            return EXIT_FAILURE;
        }
#if defined(SPATCH_FINAL_RELEASE)
        const bool summary_interval_matches_policy =
            user_names.summary_interval_ms == 0;
#else
        const bool summary_interval_matches_policy =
            user_names.summary_interval_ms == 2500;
#endif
        if (!Expect(user_names.enable_logging && summary_interval_matches_policy,
                    "PascalCase logging should parse while final builds compile "
                    "out periodic summaries")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        constexpr std::array<unsigned char, 9> crc_test{'1', '2', '3', '4', '5',
                                                        '6', '7', '8', '9'};
        if (!Expect(ComputeGameDisplayUid(std::as_bytes(std::span{crc_test})) ==
                        0x0376E6E7u,
                    "display UID hashing should match the engine CRC "
                    "implementation")) {
            return EXIT_FAILURE;
        }

        constexpr std::array<DisplayModeCandidate, 7> modes{{
            {2560, 1440, 60, 1, true},
            {2560, 1440, 144000, 1001, true},
            {2560, 1440, 288000, 2002, true},
            {2560, 1440, 240, 1, false},
            {1920, 1080, 360, 1, true},
            {2560, 1440, 999, 0, true},
            {2560, 1440, 1, 1, true},
        }};
        const auto highest = SelectHighestRefreshRate(modes, 2560, 1440);
        if (!Expect(highest.has_value() && highest->numerator == 144000 &&
                        highest->denominator == 1001,
                    "maximum refresh selection should preserve the exact "
                    "progressive rational")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!SelectHighestRefreshRate(modes, 0, 1440).has_value(),
                    "maximum refresh selection should reject an invalid "
                    "resolution")) {
            return EXIT_FAILURE;
        }
    }

    {
        std::string xml =
            "<?xml version=\"1.0\"?>\n"
            "<DisplaySettings>\n"
            "\t<ResolutionWidth>2560</ResolutionWidth>\n"
            "\t<ResolutionHeight>1440</ResolutionHeight>\n"
            "\t<RefreshRateNumerator>60</RefreshRateNumerator>\n"
            "\t<RefreshRateDenominator>1</RefreshRateDenominator>\n"
            "\t<Fullscreen>1</Fullscreen>\n"
            "</DisplaySettings>\n";

        DisplayModePreference preference{};
        preference.refresh_rate_numerator = 144;
        preference.refresh_rate_denominator = 1;
        preference.fullscreen = 0;

        if (!Expect(
                ApplyDisplayModePreference(xml, preference),
                "display mode preference should report a change when values "
                "differ")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                xml.find("<RefreshRateNumerator>144</RefreshRateNumerator>") !=
                    std::string::npos,
                "display mode preference should rewrite the refresh "
                "numerator")) {
            return EXIT_FAILURE;
        }
        if (!Expect(xml.find(
                        "<RefreshRateDenominator>1</RefreshRateDenominator>") !=
                        std::string::npos,
                    "display mode preference should preserve the refresh "
                    "denominator")) {
            return EXIT_FAILURE;
        }
        if (!Expect(xml.find("<Fullscreen>0</Fullscreen>") != std::string::npos,
                    "display mode preference should rewrite fullscreen mode")) {
            return EXIT_FAILURE;
        }
    }

    {
        std::string xml =
            "<?xml version=\"1.0\"?>\n"
            "<DisplaySettings>\n"
            "\t<Version>1</Version>\n"
            "</DisplaySettings>\n";

        DisplayModePreference preference{};
        preference.refresh_rate_numerator = 165;
        preference.fullscreen = 1;

        if (!Expect(ApplyDisplayModePreference(xml, preference),
                    "display mode preference should insert missing tags")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                xml.find("<RefreshRateNumerator>165</RefreshRateNumerator>") !=
                    std::string::npos,
                "display mode preference should insert missing refresh "
                "numerator tag")) {
            return EXIT_FAILURE;
        }
        if (!Expect(xml.find(
                        "<RefreshRateDenominator>1</RefreshRateDenominator>") !=
                        std::string::npos,
                    "display mode preference should insert a denominator when "
                    "writing "
                    "refresh")) {
            return EXIT_FAILURE;
        }
        if (!Expect(xml.find("<Fullscreen>1</Fullscreen>") != std::string::npos,
                    "display mode preference should insert missing fullscreen "
                    "tag")) {
            return EXIT_FAILURE;
        }
    }

    {
        std::string xml =
            "<DisplaySettings>\n"
            "\t<RefreshRateNumerator>60</RefreshRateNumerator>\n";
        const std::string original = xml;

        DisplayModePreference preference{};
        preference.refresh_rate_numerator = 144;
        preference.refresh_rate_denominator = 1;
        preference.fullscreen = 1;

        if (!Expect(!ApplyDisplayModePreference(xml, preference),
                    "malformed display XML should reject a multi-field "
                    "preference")) {
            return EXIT_FAILURE;
        }
        if (!Expect(xml == original,
                    "a rejected display preference must not leave a partial "
                    "rewrite")) {
            return EXIT_FAILURE;
        }
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-motion-blur-display-settings-test.xml");
        RemoveIfExists(path);
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << "<DisplaySettings><MotionBlur>2</MotionBlur>"
                      "</DisplaySettings>\n";
        }

        Config config{};
        config.time_step_smoothing = -1;
        config.override_low_res_buffer = -1;
        config.override_shadow_filter = -1;
        config.override_fps_limiter = -1;
        config.override_texture_detail_level = -1;
        config.override_world_density = -1;
        config.override_motion_blur = 0;
        ApplyDisplaySettingsPatches(path, config);

        std::ifstream disabled_stream(path, std::ios::binary);
        const std::string disabled(
            (std::istreambuf_iterator<char>(disabled_stream)), {});
        disabled_stream.close();
        if (!Expect(
                disabled.find("<MotionBlur>0</MotionBlur>") !=
                    std::string::npos,
                "MotionBlur=0 should update the game's display XML")) {
            return EXIT_FAILURE;
        }

        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << "<DisplaySettings><Version>1</Version>"
                      "</DisplaySettings>\n";
        }
        config.override_motion_blur = 1;
        ApplyDisplaySettingsPatches(path, config);
        std::ifstream inserted_stream(path, std::ios::binary);
        const std::string inserted(
            (std::istreambuf_iterator<char>(inserted_stream)), {});
        inserted_stream.close();
        if (!Expect(
                inserted.find("<MotionBlur>1</MotionBlur>") !=
                    std::string::npos,
                "MotionBlur should be inserted when the display XML omits it")) {
            return EXIT_FAILURE;
        }

        config.override_motion_blur = -1;
        ApplyDisplaySettingsPatches(path, config);
        std::ifstream preserved_stream(path, std::ios::binary);
        const std::string preserved(
            (std::istreambuf_iterator<char>(preserved_stream)), {});
        if (!Expect(
                preserved == inserted,
                "MotionBlur=-1 should preserve the game's display XML value")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-native-shadow-display-test.xml");
        RemoveIfExists(path);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream << "<DisplaySettings><ShadowFilter>0</ShadowFilter></"
                  "DisplaySettings>\n";
        stream.close();

        Config config{};
        config.override_shadow_filter = 1;
        config.time_step_smoothing = -1;
        config.override_low_res_buffer = -1;
        ApplyDisplaySettingsPatches(path, config);

        std::ifstream staged_stream(path);
        const std::string staged(
            (std::istreambuf_iterator<char>(staged_stream)), {});
        staged_stream.close();
        if (!Expect(staged.find("<ShadowFilter>1</ShadowFilter>") !=
                        std::string::npos,
                    "OriginalShadowFilter should update the native display "
                    "setting")) {
            return EXIT_FAILURE;
        }

        config.override_shadow_filter = -1;
        ApplyDisplaySettingsPatches(path, config);
        std::ifstream original_stream(path);
        const std::string original(
            (std::istreambuf_iterator<char>(original_stream)), {});
        original_stream.close();
        if (!Expect(original == staged,
                    "OriginalShadowFilter=-1 should preserve the native "
                    "display setting")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path = MakeTempIniPath(L"spatch-display-width-test.xml");
        RemoveIfExists(path);
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        stream
            << "<DisplaySettings><ResolutionWidth>3440</ResolutionWidth>"
               "<ResolutionHeight>1440</ResolutionHeight></DisplaySettings>\n";
        stream.close();
        const auto width = ReadDisplayResolutionWidth(path);
        if (!Expect(width.has_value() && *width == 3440,
                    "reflection auto mode should read the game's configured "
                    "display width")) {
            return EXIT_FAILURE;
        }

        std::ofstream malformed(path, std::ios::out | std::ios::trunc);
        malformed
            << "<DisplaySettings><ResolutionWidth>3440junk</ResolutionWidth>"
               "</DisplaySettings>\n";
        malformed.close();
        if (!Expect(
                !ReadDisplayResolutionWidth(path).has_value(),
                "display settings integers should reject trailing non-numeric "
                "text")) {
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto steam_root =
            MakeTempIniPath(L"spatch-display-path-steam");
        const auto gog_root =
            MakeTempIniPath(L"spatch-display-path-gog");
        const auto gog_first_run_root =
            MakeTempIniPath(L"spatch-display-path-gog-first-run");
        const auto gog_ambiguous_root =
            MakeTempIniPath(L"spatch-display-path-gog-ambiguous");
        const auto steam_with_save_root =
            MakeTempIniPath(L"spatch-display-path-steam-with-save");
        std::error_code error;
        std::filesystem::remove_all(steam_root, error);
        error.clear();
        std::filesystem::remove_all(gog_root, error);
        error.clear();
        std::filesystem::remove_all(gog_first_run_root, error);
        error.clear();
        std::filesystem::remove_all(gog_ambiguous_root, error);
        error.clear();
        std::filesystem::remove_all(steam_with_save_root, error);
        std::filesystem::create_directories(steam_root / L"data");
        std::filesystem::create_directories(gog_root / L"Save");
        std::filesystem::create_directories(gog_first_run_root / L"Save");
        std::filesystem::create_directories(gog_ambiguous_root / L"Save");
        std::filesystem::create_directories(gog_ambiguous_root / L"data");
        std::filesystem::create_directories(steam_with_save_root / L"Save");
        std::ofstream(steam_root / L"data" / L"DisplaySettings.xml")
            << "<DisplaySettings/>\n";
        std::ofstream(gog_root / L"Save" / L"DisplaySettings.xml")
            << "<DisplaySettings/>\n";
        std::ofstream(gog_first_run_root / L"goggame-987654321.info")
            << "{}\n";
        std::ofstream(gog_ambiguous_root / L"data" / L"DisplaySettings.xml")
            << "<DisplaySettings/>\n";
        std::ofstream(gog_ambiguous_root / L"goggame-123456789.info")
            << "{}\n";
        if (!Expect(
                ResolveDisplaySettingsPath(steam_root).lexically_normal() ==
                        (steam_root / L"data" / L"DisplaySettings.xml").lexically_normal() &&
                    ResolveDisplaySettingsPath(gog_root).lexically_normal() ==
                        (gog_root / L"Save" / L"DisplaySettings.xml").lexically_normal() &&
                    ResolveDisplaySettingsPath(gog_first_run_root).lexically_normal() ==
                        (gog_first_run_root / L"Save" / L"DisplaySettings.xml").lexically_normal() &&
                    ResolveDisplaySettingsPath(gog_ambiguous_root).lexically_normal() ==
                        (gog_ambiguous_root / L"Save" / L"DisplaySettings.xml").lexically_normal() &&
                    ResolveDisplaySettingsPath(steam_with_save_root).lexically_normal() ==
                        (steam_with_save_root / L"data" / L"DisplaySettings.xml").lexically_normal(),
                "display settings should require GOG XML or metadata and otherwise use Steam data")) {
            return EXIT_FAILURE;
        }

        error.clear();
        std::filesystem::remove_all(steam_root, error);
        error.clear();
        std::filesystem::remove_all(gog_root, error);
        error.clear();
        std::filesystem::remove_all(gog_first_run_root, error);
        error.clear();
        std::filesystem::remove_all(gog_ambiguous_root, error);
        error.clear();
        std::filesystem::remove_all(steam_with_save_root, error);
    }


    {
        const std::string legacy = IdentifyBuildFromPeMetadata(
            build_info::kKnownFileSize, build_info::kLegacyTimeDateStamp,
            build_info::kKnownSizeOfImage);
        if (!Expect(legacy == "legacy_researched",
                    "legacy metadata should match legacy build")) {
            return EXIT_FAILURE;
        }

        const std::string latest = IdentifyBuildFromPeMetadata(
            build_info::kKnownFileSize, build_info::kLatestSteamTimeDateStamp,
            build_info::kKnownSizeOfImage);
        if (!Expect(latest == "latest_steam",
                    "latest metadata should match latest steam build")) {
            return EXIT_FAILURE;
        }

        const std::string unknown =
            IdentifyBuildFromPeMetadata(build_info::kKnownFileSize, 0xDEADBEEF,
                                        build_info::kKnownSizeOfImage);
        if (!Expect(unknown == "unknown",
                    "unknown metadata should stay unknown")) {
            return EXIT_FAILURE;
        }
    }

    {
        const std::filesystem::path long_absolute =
            std::filesystem::path(L"C:\\") /
            std::wstring(300, L'a') / L"SPatch.asi";
        if (!Expect(
                !IsUsableBootstrapModulePath({}) &&
                    !IsUsableBootstrapModulePath(L"SPatch.asi") &&
                    !IsUsableBootstrapModulePath(L"mods\\SPatch.asi") &&
                    !IsUsableBootstrapModulePath(L"C:\\") &&
                    IsUsableBootstrapModulePath(
                        L"C:\\Games\\SleepingDogs\\SPatch.asi") &&
                    IsUsableBootstrapModulePath(long_absolute),
                "bootstrap should reject empty or relative module paths and "
                "accept absolute module-file paths before filesystem setup")) {
            return EXIT_FAILURE;
        }
    }

    {
        BuildCheckResult unsupported_allowed{};
        unsupported_allowed.supported = false;
        unsupported_allowed.hook_layout_supported = false;
        if (!Expect(ShouldUseSafeCompatibilityMode(unsupported_allowed, true),
                    "allowed unsupported builds should use safe compatibility "
                    "mode")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!ShouldUseSafeCompatibilityMode(unsupported_allowed, false),
                    "disallowed unsupported builds should not enter safe "
                    "compatibility mode")) {
            return EXIT_FAILURE;
        }

        BuildCheckResult supported_layout{};
        supported_layout.supported = true;
        supported_layout.hook_layout_supported = true;
        if (!Expect(!ShouldUseSafeCompatibilityMode(supported_layout, true),
                    "supported mapped builds should not use safe compatibility "
                    "mode")) {
            return EXIT_FAILURE;
        }
    }

    {
        Config config{};
        config.allow_unverified_build = true;
        config.hook_frameflow = true;
        config.hook_nis_timing = true;
        config.hook_wait_helper = true;
        config.hook_scaleform_time = true;
        config.hook_scaleform_init = true;
        config.hook_nis_runtime = true;
        config.hook_nis_actor_state = true;
        config.hook_twitch_probe = true;
        config.hook_scenery_builders = true;
        config.hook_aa_probe = true;
        config.hook_aa_fx_probe = true;
        config.aa_variant_debug_keys = true;
        config.aa_aux_debug_keys = true;
        config.hook_post_material_submit = true;
        config.override_rumble_enabled = 1;
        config.force_raw_mouse_input = true;
        config.disable_camera_smoothing = true;
        config.gta_iv_car_camera = true;
        config.gta_iv_bike_camera = true;
        config.controller_left_stick_deadzone = 0;
        config.controller_right_stick_deadzone = 15;
        config.fix_nis_actor_restore_duplicates = true;
        config.smaa_enable = true;
        config.restore_character_wetness = true;
        config.restore_character_sweat = true;
        config.restore_original_fog = true;
        config.restore_original_eye_reflections = true;
        config.hook_character_regression_probe = true;

        BuildCheckResult unsupported_allowed{};
        unsupported_allowed.supported = false;
        unsupported_allowed.hook_layout_supported = false;
        unsupported_allowed.build_id = "unknown";

        const HookInstallPlan safe_plan =
            BuildHookInstallPlan(config, unsupported_allowed);
        if (!Expect(!safe_plan.install_hooks,
                    "allowed unsupported builds should remain settings-only")) {
            return EXIT_FAILURE;
        }
        if (!Expect(safe_plan.safe_compatibility_mode,
                    "allowed unsupported builds should resolve to safe "
                    "compatibility mode")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                    !safe_plan.effective_config.hook_frameflow &&
                    !safe_plan.effective_config.hook_nis_timing &&
                    !safe_plan.effective_config.force_raw_mouse_input &&
                    !safe_plan.effective_config.disable_camera_smoothing &&
                    !safe_plan.effective_config.gta_iv_car_camera &&
                    !safe_plan.effective_config.gta_iv_bike_camera &&
                    safe_plan.effective_config.controller_left_stick_deadzone ==
                        input::kStockDeadzone &&
                    safe_plan.effective_config
                            .controller_right_stick_deadzone ==
                        input::kStockDeadzone &&
                    !safe_plan.effective_config.hook_nis_actor_state &&
                    !safe_plan.effective_config.hook_twitch_probe &&
                    !safe_plan.effective_config
                         .fix_nis_actor_restore_duplicates &&
                    safe_plan.effective_config.override_rumble_enabled == -1 &&
                     !safe_plan.effective_config.smaa_enable &&
                     !safe_plan.effective_config.restore_original_fog &&
                     !safe_plan.effective_config.restore_character_wetness &&
                    !safe_plan.effective_config.restore_character_sweat &&
                    !safe_plan.effective_config
                         .restore_original_eye_reflections &&
                    !safe_plan.effective_config
                         .fix_pedestrian_density_at_high_fps &&
                    !safe_plan.effective_config.improve_spherical_reflections &&
                    safe_plan.effective_config.anisotropic_filtering == -1 &&
                    !safe_plan.effective_config.force_anisotropic_filtering &&
                    !safe_plan.effective_config.remove_hidden_120_fps_cap &&
                    !safe_plan.effective_config.fix_first_run_resolution &&
                    !safe_plan.effective_config.fix_scaleform_qpc_clock &&
                    !safe_plan.effective_config.fix_file_timestamp_open_mode &&
                    !safe_plan.effective_config.fix_audio_file_open &&
                    !safe_plan.effective_config.fix_large_file_sizes &&
                    !safe_plan.effective_config.fix_vram_pool_lock &&
                    !safe_plan.effective_config.fix_vram_capacity_reporting &&
                    !safe_plan.effective_config.fix_resource_loading &&
                    !safe_plan.effective_config.fix_contact_list_overflow &&
                    !safe_plan.effective_config.fix_corrupt_save_handling &&
                    !safe_plan.effective_config.fix_thread_creation_failure &&
                    !safe_plan.effective_config.fix_high_fps_average_windows,
                "safe compatibility mode should disable hook-heavy features "
                "without fabricating requested setting values")) {
            return EXIT_FAILURE;
        }

        BuildCheckResult metadata_only{};
        metadata_only.supported = true;
        metadata_only.hook_layout_supported = false;
        metadata_only.build_id = "legacy_metadata_only";
        const HookInstallPlan metadata_plan =
            BuildHookInstallPlan(config, metadata_only);
        if (!Expect(
                !metadata_plan.install_hooks &&
                    metadata_plan.safe_compatibility_mode &&
                    !metadata_plan.effective_config.restore_original_fog &&
                    !metadata_plan.effective_config.hook_frameflow,
                "metadata-only build recognition must never re-enable a "
                "fixed-RVA fog, frameflow, or AO hook")) {
            return EXIT_FAILURE;
        }

        BuildCheckResult legacy_researched{};
        legacy_researched.supported = true;
        legacy_researched.hook_layout_supported = true;
        legacy_researched.build_id = "legacy_researched";
        const HookInstallPlan legacy_plan =
            BuildHookInstallPlan(config, legacy_researched);
        if (!Expect(
                legacy_plan.install_hooks &&
                    !legacy_plan.safe_compatibility_mode &&
                    !legacy_plan.latest_steam_profile &&
                    legacy_plan.effective_config.gta_iv_car_camera &&
                    legacy_plan.effective_config.gta_iv_bike_camera,
                "the mapped legacy profile should preserve both enabled GTA "
                "IV vehicle-camera requests")) {
            return EXIT_FAILURE;
        }

        BuildCheckResult latest_steam{};
        latest_steam.supported = true;
        latest_steam.hook_layout_supported = true;
        latest_steam.build_id = "latest_steam";

        const HookInstallPlan latest_plan =
            BuildHookInstallPlan(config, latest_steam);
        if (!Expect(
                latest_plan.install_hooks,
                "latest steam builds should proceed to hook initialization")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                latest_plan.latest_steam_profile,
                "latest steam builds should apply the compatibility profile")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                !latest_plan.effective_config.hook_character_regression_probe &&
                    !latest_plan.effective_config.hook_wait_helper &&
                    !latest_plan.effective_config.hook_scaleform_time &&
                    !latest_plan.effective_config.hook_scaleform_init &&
                    !latest_plan.effective_config.hook_nis_runtime &&
                    !latest_plan.effective_config.hook_nis_actor_state &&
                    !latest_plan.effective_config.hook_twitch_probe &&
                    !latest_plan.effective_config.hook_scenery_builders &&
                    !latest_plan.effective_config.hook_aa_probe &&
                    !latest_plan.effective_config.hook_aa_fx_probe &&
                    !latest_plan.effective_config.hook_smaa_present &&
                    !latest_plan.effective_config.smaa_enable &&
                    !latest_plan.effective_config.hook_post_material_submit &&
                    !latest_plan.effective_config
                         .fix_nis_actor_restore_duplicates &&
                    latest_plan.effective_config.restore_character_wetness &&
                    !latest_plan.effective_config.restore_character_sweat &&
                    !latest_plan.effective_config.gta_iv_car_camera &&
                    !latest_plan.effective_config.gta_iv_bike_camera &&
                    latest_plan.effective_config
                        .restore_original_eye_reflections &&
                    latest_plan.effective_config.override_rumble_enabled == -1,
                "latest steam profile should retain only mapped features and "
                "disable sweat and both vehicle-camera mutations without live "
                "path evidence")) {
            return EXIT_FAILURE;
        }

        for (const auto [car_value, bike_value] :
             std::array{std::pair{false, false}, std::pair{false, true},
                        std::pair{true, false}, std::pair{true, true}}) {
            Config combination_config = config;
            combination_config.gta_iv_car_camera = car_value;
            combination_config.gta_iv_bike_camera = bike_value;
            const HookInstallPlan combination_safe =
                BuildHookInstallPlan(combination_config, unsupported_allowed);
            const HookInstallPlan combination_legacy =
                BuildHookInstallPlan(combination_config, legacy_researched);
            const HookInstallPlan combination_latest =
                BuildHookInstallPlan(combination_config, latest_steam);
            if (!Expect(
                    !combination_safe.effective_config.gta_iv_car_camera &&
                        !combination_safe.effective_config
                             .gta_iv_bike_camera &&
                        combination_legacy.effective_config
                                .gta_iv_car_camera == car_value &&
                        combination_legacy.effective_config
                                .gta_iv_bike_camera == bike_value &&
                        !combination_latest.effective_config
                             .gta_iv_car_camera &&
                        !combination_latest.effective_config
                             .gta_iv_bike_camera,
                    "safe and latest-Steam profiles should force both camera "
                    "flags off while mapped legacy preserves every requested "
                    "car/bike combination")) {
                return EXIT_FAILURE;
            }
        }

        BuildCheckResult blocked_unsupported{};
        blocked_unsupported.supported = false;
        blocked_unsupported.hook_layout_supported = false;
        blocked_unsupported.build_id = "unknown";

        config.allow_unverified_build = false;
        const HookInstallPlan blocked_plan =
            BuildHookInstallPlan(config, blocked_unsupported);
        if (!Expect(!blocked_plan.install_hooks,
                    "disallowed unsupported builds should block hook "
                    "initialization")) {
            return EXIT_FAILURE;
        }
    }

    {
        Config config{};
        config.enabled = false;

        BuildCheckResult build{};
        build.supported = true;
        build.hook_layout_supported = true;
        build.build_id = "legacy_researched";

        const BootstrapPrepareResult disabled =
            PrepareBootstrapHooks(config, build, false);
        if (!Expect(disabled.status == BootstrapPrepareStatus::DisabledByConfig,
                    "disabled config should stop bootstrap before hooks")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!ShouldApplyDisplaySettings(disabled.status),
                    "disabled bootstrap must not rewrite display settings")) {
            return EXIT_FAILURE;
        }

        config.enabled = true;
        const BootstrapPrepareResult detached =
            PrepareBootstrapHooks(config, build, true);
        if (!Expect(
                detached.status == BootstrapPrepareStatus::DetachedBeforeHooks,
                "detaching after inspect should stop bootstrap before hooks")) {
            return EXIT_FAILURE;
        }
        if (!Expect(!ShouldApplyDisplaySettings(detached.status),
                    "detaching bootstrap must not rewrite display settings")) {
            return EXIT_FAILURE;
        }

        const BootstrapPrepareResult ready =
            PrepareBootstrapHooks(config, build, false);
        if (!Expect(ready.status ==
                        BootstrapPrepareStatus::ReadyForHookInitialization,
                    "supported mapped builds should proceed to hook "
                    "initialization")) {
            return EXIT_FAILURE;
        }
        if (!Expect(ShouldApplyDisplaySettings(ready.status),
                    "accepted mapped builds may apply display settings")) {
            return EXIT_FAILURE;
        }

        BuildCheckResult unsupported{};
        unsupported.supported = false;
        unsupported.hook_layout_supported = false;
        const BootstrapPrepareResult rejected =
            PrepareBootstrapHooks(config, unsupported, false);
        if (!Expect(
                rejected.status == BootstrapPrepareStatus::UnsupportedBuild &&
                    !ShouldApplyDisplaySettings(rejected.status),
                "a rejected executable must not rewrite DisplaySettings.xml")) {
            return EXIT_FAILURE;
        }
        config.allow_unverified_build = true;
        const BootstrapPrepareResult safe =
            PrepareBootstrapHooks(config, unsupported, false);
        if (!Expect(safe.status ==
                        BootstrapPrepareStatus::ReadyForSettingsOnly &&
                        safe.safe_compatibility_mode &&
                        ShouldApplyDisplaySettings(safe.status),
                    "explicit safe compatibility may apply build-independent "
                    "display settings without initializing hooks")) {
            return EXIT_FAILURE;
        }
        if (!Expect(FinalizeBootstrapHooks(true) ==
                        BootstrapFinalizeStatus::Initialized,
                    "successful hook initialization should finalize as "
                    "initialized")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                FinalizeBootstrapHooks(false) ==
                    BootstrapFinalizeStatus::HookInitializationFailed,
                "failed hook initialization should finalize as a failure")) {
            return EXIT_FAILURE;
        }
    }

    {
        const auto path = MakeTempIniPath(L"spatch-sha-read-failure-test.bin");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "spatch";
        stream.close();

        std::array<unsigned char, 32> hash{};
        if (!Expect(!ComputeSha256FromPathWithReadCallback(path, hash,
                                                           &FailReadFile),
                    "hashing should fail when the read callback fails")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        if (!Expect(!ComputeSha256FromPathWithReadCallback(path, hash,
                                                           &OversizedReadFile),
                    "hashing should reject a callback count larger than its "
                    "buffer")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path = MakeTempIniPath(L"spatch-pe-sha-fallback-test.exe");
        RemoveIfExists(path);
        if (!Expect(WriteMinimalPeFile(path, 0x11111111u, 0x2000u),
                    "should be able to write a minimal PE test file")) {
            return EXIT_FAILURE;
        }

        const BuildCheckResult result =
            InspectGameAtPathWithHashCallback(path, &ReturnLegacyHash);
        if (!Expect(
                result.supported,
                "SHA fallback should classify the minimal PE as supported")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        if (!Expect(result.hook_layout_supported,
                    "SHA fallback should mark hook layout as supported")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        if (!Expect(result.hash_computed,
                    "SHA fallback should report a computed hash")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        if (!Expect(result.build_id == "legacy_researched",
                    "SHA fallback should classify the legacy build id")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        if (!Expect(result.summary.find("identity_path=sha256") !=
                        std::string::npos,
                    "SHA fallback summary should report sha256 identity")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path = MakeTempIniPath(L"spatch-pe-metadata-only-test.exe");
        RemoveIfExists(path);
        if (!Expect(WriteMinimalPeFile(path, build_info::kLegacyTimeDateStamp,
                                       build_info::kKnownSizeOfImage,
                                       IMAGE_FILE_MACHINE_AMD64,
                                       build_info::kKnownFileSize),
                    "should be able to write a metadata-only PE fixture")) {
            return EXIT_FAILURE;
        }

        const BuildCheckResult result =
            InspectGameAtPathWithHashCallback(path, &ReturnHashFailure);
        if (!Expect(
                result.supported && !result.hook_layout_supported &&
                    result.summary.find("identity_path=metadata_unverified") !=
                        std::string::npos &&
                    result.summary.find("hook_layout=safe_mode") !=
                        std::string::npos,
                "metadata-only recognition should report the safe hook "
                "policy")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

    {
        const auto path =
            MakeTempIniPath(L"spatch-pe-wrong-architecture-test.exe");
        RemoveIfExists(path);
        if (!Expect(
                WriteMinimalPeFile(path, 0x11111111u, 0x2000u,
                                   IMAGE_FILE_MACHINE_I386),
                "should be able to write a wrong-architecture PE fixture")) {
            return EXIT_FAILURE;
        }

        const BuildCheckResult result =
            InspectGameAtPathWithHashCallback(path, &ReturnLegacyHash);
        if (!Expect(!result.supported && !result.hook_layout_supported,
                    "a non-x64 PE must never authorize fixed x64 hooks")) {
            RemoveIfExists(path);
            return EXIT_FAILURE;
        }
        RemoveIfExists(path);
    }

#if !defined(SPATCH_FINAL_RELEASE)
    {
        hooks::SummarySnapshot snapshot{};
        snapshot.task_ready = 1;
        snapshot.task_dispatch = 2;
        snapshot.nis_scene_fix = 7;
        snapshot.nis_actor_setup = 4;
        snapshot.nis_actor_restore_untracked = 5;
        snapshot.nis_actor_setup_duplicate = 9;
        snapshot.nis_actor_restore_duplicate = 10;
        snapshot.nis_actor_restore_never_seen = 11;
        snapshot.nis_actor_restore_suppressed = 12;
        snapshot.twitch_tick = 6;
        snapshot.twitch_login_callback = 7;
        snapshot.twitch_login_failure = 8;
        snapshot.cutscene_flow_fix_paused = 6;
        snapshot.smaa_width = 1920;
        snapshot.smaa_height = 1080;
        snapshot.provider_ptr = 0x1234;

        const std::string line = hooks::FormatSummaryMessage(snapshot);
        if (!Expect(line.starts_with("summary task_ready=1 task_dispatch=2"),
                    "summary line should start with the task counters")) {
            return EXIT_FAILURE;
        }
        if (!Expect(line.find("nis_scene_fix=7") != std::string::npos,
                    "summary line should include nis_scene_fix")) {
            return EXIT_FAILURE;
        }
        if (!Expect(line.find("nis_actor_setup=4") != std::string::npos &&
                        line.find("nis_actor_restore_untracked=5") !=
                            std::string::npos &&
                        line.find("nis_actor_setup_duplicate=9") !=
                            std::string::npos &&
                        line.find("nis_actor_restore_duplicate=10") !=
                            std::string::npos &&
                        line.find("nis_actor_restore_never_seen=11") !=
                            std::string::npos &&
                        line.find("nis_actor_restore_suppressed=12") !=
                            std::string::npos,
                    "summary line should include NIS actor-state counters")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                line.find("twitch_tick=6") != std::string::npos &&
                    line.find("twitch_login_callback=7") != std::string::npos &&
                    line.find("twitch_login_failure=8") != std::string::npos,
                "summary line should include Twitch probe counters")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                line.find("cutscene_flow_fix_paused=6") != std::string::npos,
                "summary line should include paused cutscene fix counters")) {
            return EXIT_FAILURE;
        }
        if (!Expect(line.find("smaa_size=1920x1080") != std::string::npos,
                    "summary line should include SMAA dimensions")) {
            return EXIT_FAILURE;
        }
        if (!Expect(line.find("provider_ptr=0x") != std::string::npos,
                    "summary line should include provider_ptr")) {
            return EXIT_FAILURE;
        }
        if (!Expect(line.size() == std::strlen(line.c_str()),
                    "summary formatter should not leave embedded NUL bytes")) {
            return EXIT_FAILURE;
        }
    }

    {
        hooks::SummaryRuntimeFields fields{};
        fields.snapshot.task_ready = 11;
        fields.snapshot.nis_scene_fix = 22;
        fields.snapshot.nis_actor_setup = 33;
        fields.snapshot.nis_actor_restore = 44;
        fields.snapshot.nis_actor_restore_untracked = 55;
        fields.snapshot.nis_actor_setup_duplicate = 66;
        fields.snapshot.nis_actor_restore_duplicate = 77;
        fields.snapshot.nis_actor_restore_never_seen = 88;
        fields.snapshot.nis_actor_restore_suppressed = 99;
        fields.snapshot.char_health_damage = 17;
        fields.nis_last_bits = FloatToBits(1.25f);
        fields.cutscene_flow_fwd_bits = FloatToBits(2.5f);
        fields.smaa_stats.enabled = true;
        fields.smaa_stats.any_hook_retained = true;
        fields.smaa_stats.resources_ready = true;
        fields.smaa_stats.present_count = 10;
        fields.smaa_stats.apply_count = 20;
        fields.smaa_stats.fail_count = 30;
        fields.smaa_stats.resize_count = 40;
        fields.smaa_stats.width = 2560;
        fields.smaa_stats.height = 1440;
        fields.provider_ptr = 0xCAFEu;

        const hooks::SummarySnapshot snapshot =
            hooks::BuildSummarySnapshot(fields);
        if (!Expect(snapshot.task_ready == 11 && snapshot.nis_scene_fix == 22,
                    "runtime summary builder should preserve direct counter "
                    "fields")) {
            return EXIT_FAILURE;
        }
        if (!Expect(snapshot.nis_actor_setup == 33 &&
                        snapshot.nis_actor_restore == 44 &&
                        snapshot.nis_actor_restore_untracked == 55 &&
                        snapshot.nis_actor_setup_duplicate == 66 &&
                        snapshot.nis_actor_restore_duplicate == 77 &&
                        snapshot.nis_actor_restore_never_seen == 88 &&
                        snapshot.nis_actor_restore_suppressed == 99,
                    "runtime summary builder should preserve NIS actor-state "
                    "counters")) {
            return EXIT_FAILURE;
        }
        if (!Expect(snapshot.char_health_damage == 17,
                    "runtime summary builder should preserve direct state "
                    "fields")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                snapshot.nis_last == 1.25f &&
                    snapshot.cutscene_flow_fwd == 2.5f,
                "runtime summary builder should decode raw float bit fields")) {
            return EXIT_FAILURE;
        }
        if (!Expect(snapshot.smaa_enabled == 1 &&
                        snapshot.smaa_any_hook_retained == 1 &&
                        snapshot.smaa_ready == 1,
                    "runtime summary builder should map SMAA boolean state")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                snapshot.smaa_present == 10 && snapshot.smaa_apply == 20 &&
                    snapshot.smaa_fail == 30 && snapshot.smaa_resize == 40 &&
                    snapshot.smaa_width == 2560 && snapshot.smaa_height == 1440,
                "runtime summary builder should map SMAA counters and "
                "dimensions")) {
            return EXIT_FAILURE;
        }
        if (!Expect(snapshot.provider_ptr == 0xCAFEu,
                    "runtime summary builder should preserve the provider "
                    "pointer")) {
            return EXIT_FAILURE;
        }
    }
#endif

    {
        hooks::nisprobe::Tracker tracker{};

        const auto setup =
            hooks::nisprobe::TrackSetup(tracker, 0x1000u, 0x2000u);
        if (!Expect(!setup.duplicate && setup.active_count == 1 &&
                        setup.setup_count == 1 && setup.restore_count == 0 &&
                        setup.last_target == 0x2000u,
                    "first setup should activate and initialize lifecycle "
                    "tracking")) {
            return EXIT_FAILURE;
        }

        const auto duplicate_setup =
            hooks::nisprobe::TrackSetup(tracker, 0x1000u, 0x3000u);
        if (!Expect(duplicate_setup.duplicate &&
                        duplicate_setup.active_count == 1 &&
                        duplicate_setup.setup_count == 2 &&
                        duplicate_setup.restore_count == 0 &&
                        duplicate_setup.last_target == 0x3000u,
                    "second setup without restore should be classified as "
                    "duplicate")) {
            return EXIT_FAILURE;
        }

        const auto first_restore =
            hooks::nisprobe::TrackRestore(tracker, 0x1000u);
        if (!Expect(
                first_restore.disposition ==
                        hooks::nisprobe::RestoreDisposition::tracked &&
                    first_restore.active_count == 0 &&
                    first_restore.setup_count == 2 &&
                    first_restore.restore_count == 1 &&
                    first_restore.last_target == 0x3000u,
                "first restore after setup should be classified as tracked")) {
            return EXIT_FAILURE;
        }

        const auto duplicate_restore =
            hooks::nisprobe::TrackRestore(tracker, 0x1000u);
        if (!Expect(
                duplicate_restore.disposition ==
                        hooks::nisprobe::RestoreDisposition::duplicate &&
                    duplicate_restore.active_count == 0 &&
                    duplicate_restore.setup_count == 2 &&
                    duplicate_restore.restore_count == 2 &&
                    duplicate_restore.last_target == 0x3000u,
                "second restore without a new setup should be classified as "
                "duplicate")) {
            return EXIT_FAILURE;
        }

        const auto never_seen_restore =
            hooks::nisprobe::TrackRestore(tracker, 0x4000u);
        if (!Expect(never_seen_restore.disposition ==
                            hooks::nisprobe::RestoreDisposition::never_seen &&
                        never_seen_restore.active_count == 0 &&
                        never_seen_restore.setup_count == 0 &&
                        never_seen_restore.restore_count == 0 &&
                        never_seen_restore.last_target == 0,
                    "restore for an unseen state should be classified "
                    "separately")) {
            return EXIT_FAILURE;
        }

        hooks::nisprobe::ResetTracker(tracker);
        if (!Expect(
                tracker.live_count == 0 && tracker.states.empty(),
                "reset should clear all tracked actor-state lifecycle data")) {
            return EXIT_FAILURE;
        }
        if (!Expect(tracker.touch_sequence == 0,
                    "reset should clear actor-state retention ordering")) {
            return EXIT_FAILURE;
        }
    }

    {
        hooks::nisprobe::Tracker tracker{};
        for (std::size_t index = 1;
             index <= hooks::nisprobe::kMaxRetainedActorStates; ++index) {
            hooks::nisprobe::TrackSetup(tracker, index, index + 1);
            hooks::nisprobe::TrackRestore(tracker, index);
        }
        hooks::nisprobe::TrackSetup(
            tracker, hooks::nisprobe::kMaxRetainedActorStates + 1, 1);
        if (!Expect(tracker.states.size() ==
                        hooks::nisprobe::kMaxRetainedActorStates,
                    "actor-state duplicate tracking should evict old inactive "
                    "tombstones")) {
            return EXIT_FAILURE;
        }
        if (!Expect(tracker.live_count == 1,
                    "bounded actor-state retention must "
                    "preserve the newly live actor")) {
            return EXIT_FAILURE;
        }

        hooks::nisprobe::Tracker all_live{};
        for (std::size_t index = 1;
             index <= hooks::nisprobe::kMaxRetainedActorStates; ++index) {
            hooks::nisprobe::TrackSetup(all_live, index, index + 1);
        }
        const auto untracked = hooks::nisprobe::TrackSetup(
            all_live, hooks::nisprobe::kMaxRetainedActorStates + 1, 1);
        if (!Expect(
                all_live.states.size() ==
                        hooks::nisprobe::kMaxRetainedActorStates &&
                    all_live.live_count ==
                        hooks::nisprobe::kMaxRetainedActorStates &&
                    !untracked.duplicate &&
                    untracked.active_count ==
                        hooks::nisprobe::kMaxRetainedActorStates,
                "all-live actor-state retention must stay bounded instead of "
                "growing the map")) {
            return EXIT_FAILURE;
        }
    }

#if !defined(SPATCH_FINAL_RELEASE)
    {
        constexpr auto u64 = std::numeric_limits<unsigned long long>::max();
        constexpr auto u32 = std::numeric_limits<unsigned int>::max();
        constexpr auto i32 = std::numeric_limits<int>::max();
        constexpr auto ptr = std::numeric_limits<std::uintptr_t>::max();

        hooks::SummarySnapshot snapshot{};
        snapshot.task_ready = u64;
        snapshot.task_dispatch = u64;
        snapshot.wait_helper = u64;
        snapshot.wait_task = u64;
        snapshot.wait_gt16 = u64;
        snapshot.wait_gt100 = u64;
        snapshot.wait_gt1000 = u64;
        snapshot.wait_gt5000 = u64;
        snapshot.scaleform_time = u64;
        snapshot.provider_non_null = u64;
        snapshot.scaleform_init = u64;
        snapshot.nis_time = u64;
        snapshot.nis_sync = u64;
        snapshot.nis_dt0 = u64;
        snapshot.nis_dt30 = u64;
        snapshot.nis_dt60 = u64;
        snapshot.nis_dt_other = u64;
        snapshot.nis_scene_fix = u64;
        snapshot.nis_play = u64;
        snapshot.nis_play_adv = u64;
        snapshot.nis_play_repeat = u64;
        snapshot.nis_play_multi = u64;
        snapshot.nis_boot = u64;
        snapshot.nis_boot_s1 = u64;
        snapshot.nis_boot_s2 = u64;
        snapshot.nis_boot_fail = u64;
        snapshot.nis_owner = u64;
        snapshot.nis_owner_dt0 = u64;
        snapshot.nis_owner_dt30 = u64;
        snapshot.nis_owner_dt60 = u64;
        snapshot.nis_owner_dt_other = u64;
        snapshot.nis_owner_adv = u64;
        snapshot.nis_owner_repeat = u64;
        snapshot.nis_owner_multi = u64;
        snapshot.nis_actor_setup = u64;
        snapshot.nis_actor_restore = u64;
        snapshot.nis_actor_restore_untracked = u64;
        snapshot.nis_actor_setup_duplicate = u64;
        snapshot.nis_actor_restore_duplicate = u64;
        snapshot.nis_actor_restore_never_seen = u64;
        snapshot.nis_actor_restore_suppressed = u64;
        snapshot.twitch_tick = u64;
        snapshot.twitch_login_callback = u64;
        snapshot.twitch_login_failure = u64;
        snapshot.nis_last = 999999.0f;
        snapshot.nis_last_delta = 999999.0f;
        snapshot.nis_owner_last_dt = 999999.0f;
        snapshot.frameflow = u64;
        snapshot.frameflow_dt0 = u64;
        snapshot.frameflow_dt60 = u64;
        snapshot.frameflow_dt_other = u64;
        snapshot.frameflow_from_cutscene = u64;
        snapshot.frameflow_last_dt = 999999.0f;
        snapshot.cutscene_flow = u64;
        snapshot.cutscene_flow_dt0 = u64;
        snapshot.cutscene_flow_fwd0 = u64;
        snapshot.cutscene_flow_fwd60 = u64;
        snapshot.cutscene_flow_fwd_other = u64;
        snapshot.cutscene_flow_fix = u64;
        snapshot.cutscene_flow_fix_paused = u64;
        snapshot.cutscene_flow_in = 999999.0f;
        snapshot.cutscene_flow_fwd = 999999.0f;
        snapshot.fog_slicing = u64;
        snapshot.fog_clamps = u64;
        snapshot.aa_owner = u64;
        snapshot.aa_skip = u64;
        snapshot.aa_main = u64;
        snapshot.aa_hairblur0 = u64;
        snapshot.aa_state = i32;
        snapshot.aa_hair_gate = i32;
        snapshot.aa_variant = i32;
        snapshot.aa_variant_apply = u64;
        snapshot.aa_aux_mode = i32;
        snapshot.aa_aux_apply = u64;
        snapshot.aa_shader = u32;
        snapshot.aa_raster = u32;
        snapshot.aa_aux = u32;
        snapshot.aa_material = ptr;
        snapshot.aa_target = ptr;
        snapshot.aa_src_a = ptr;
        snapshot.aa_src_b = ptr;
        snapshot.aa_fx = u64;
        snapshot.aa_fx_arg1 = ptr;
        snapshot.aa_fx_arg2 = ptr;
        snapshot.aa_fx_arg3 = ptr;
        snapshot.smaa_enabled = i32;
        snapshot.smaa_any_hook_retained = i32;
        snapshot.smaa_ready = i32;
        snapshot.smaa_present = u64;
        snapshot.smaa_apply = u64;
        snapshot.smaa_fail = u64;
        snapshot.smaa_resize = u64;
        snapshot.smaa_width = u32;
        snapshot.smaa_height = u32;
        snapshot.rumble_override = u64;
        snapshot.rumble_value = i32;
        snapshot.post_submit = u64;
        snapshot.post_comp_lights = u64;
        snapshot.post_comp_final = u64;
        snapshot.post_bloom = u64;
        snapshot.post_lightshaft = u64;
        snapshot.post_shadow_collector = u64;
        snapshot.post_final_chg = u64;
        snapshot.post_final_flags = i32;
        snapshot.post_final_cmd = ptr;
        snapshot.post_final_params = ptr;
        snapshot.post_final_p0 = ptr;
        snapshot.post_final_p1 = ptr;
        snapshot.post_final_p2 = ptr;
        snapshot.post_final_p3 = ptr;
        snapshot.char_water = u64;
        snapshot.char_water_speed = 999999.0f;
        snapshot.char_fx_update = u64;
        snapshot.char_fx_wet_updates = u64;
        snapshot.char_fx_surface = u32;
        snapshot.char_fx_wet_surface = u32;
        snapshot.char_fx_gate = i32;
        snapshot.char_fx_tod_weather = 999999.0f;
        snapshot.char_fx_tod_override = 999999.0f;
        snapshot.char_fx_onfire = u32;
        snapshot.char_fx_smolder = u32;
        snapshot.char_fx_attached = u32;
        snapshot.char_fx_fire_time = 999999.0f;
        snapshot.char_fx_smolder_time = 999999.0f;
        snapshot.char_fx_queued_damage = 999999.0f;
        snapshot.char_health_apply = u64;
        snapshot.char_health_proj = u64;
        snapshot.char_health_melee = u64;
        snapshot.char_health_anim_found = u64;
        snapshot.char_health_hitreact_found = u64;
        snapshot.char_health_damage = i32;
        snapshot.char_health_projectile = u32;
        snapshot.char_health_component = ptr;
        snapshot.char_health_anim = ptr;
        snapshot.char_health_hitreact = ptr;
        snapshot.char_health_attacker = ptr;
        snapshot.char_health_hit = ptr;
        snapshot.char_wet_force = u64;
        snapshot.char_wet_force_verify = u64;
        snapshot.char_charred_anim = u64;
        snapshot.char_charred_rig = u64;
        snapshot.char_charred_amount = 999999.0f;
        snapshot.char_dispatch_owner = u64;
        snapshot.char_dispatch_consume = u64;
        snapshot.char_dispatch_owner_ptr = ptr;
        snapshot.char_dispatch_component = ptr;
        snapshot.char_queue_build = u64;
        snapshot.char_queue_build_tracked = u64;
        snapshot.char_queue_build_owner = ptr;
        snapshot.char_queue_build_component = ptr;
        snapshot.char_queue_build_mode = u32;
        snapshot.char_paint_owner = u64;
        snapshot.char_paint_owner_ptr = ptr;
        snapshot.char_paint_owner_component = ptr;
        snapshot.char_paint_consumer = u64;
        snapshot.char_paint_anim = u64;
        snapshot.char_paint_rig = u64;
        snapshot.char_paint_enable = u32;
        snapshot.char_paint_r = 999999.0f;
        snapshot.char_paint_g = 999999.0f;
        snapshot.char_paint_b = 999999.0f;
        snapshot.char_damage_create = u64;
        snapshot.char_damage_reset = u64;
        snapshot.post_last = ptr;
        snapshot.unique_callbacks = u64;
        snapshot.scenery_prepare = u64;
        snapshot.scenery_setup = u64;
        snapshot.render_scenery = u64;
        snapshot.rasterize_bucket = u64;
        snapshot.scenery_prepare_ready = u64;
        snapshot.scenery_setup_ready = u64;
        snapshot.render_scenery_ready = u64;
        snapshot.rasterize_bucket_ready = u64;
        snapshot.scenery_setup_qdelta = u64;
        snapshot.render_scenery_qdelta = u64;
        snapshot.rasterize_bucket_qdelta = u64;
        snapshot.scenery_count0 = u32;
        snapshot.scenery_count1 = u32;
        snapshot.scenery_count2 = u32;
        snapshot.scenery_count3 = u32;
        snapshot.provider_ptr = ptr;

        const std::string line = hooks::FormatSummaryMessage(snapshot);
        if (!Expect(line.size() > 4096,
                    "summary formatter stress case should "
                    "exceed the initial buffer size")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                line.size() == std::strlen(line.c_str()),
                "summary formatter stress case should not leave embedded NUL "
                "bytes")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                line.find("provider_ptr=0x") != std::string::npos,
                "summary formatter stress case should preserve trailing fields "
                "after resize")) {
            return EXIT_FAILURE;
        }
        if (!Expect(
                line.find("cutscene_flow_fix_paused=") != std::string::npos,
                "summary formatter stress case should include paused cutscene "
                "fix counters")) {
            return EXIT_FAILURE;
        }
    }
#endif

    std::cout << "SPatch ConfigVersionTests passed\n";
    return EXIT_SUCCESS;
}
