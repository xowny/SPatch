#include "HookTargetGuard.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace spatch::hook_guard {
namespace {

constexpr std::size_t kMaximumPeSections = 96;

template <typename T>
bool ReadObject(std::span<const std::uint8_t> bytes, std::size_t offset, T& value) noexcept {
    if (!IsContainedRange(offset, sizeof(T), bytes.size())) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}

bool SafeCopyFromImage(const void* source, void* destination, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsExecutableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    switch (protection & 0xFFU) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool ValidateExecutableMemoryRange(std::uintptr_t start,
                                   std::size_t length,
                                   std::uintptr_t module_base,
                                   std::uint32_t& win32_error) noexcept {
    if (length == 0 || start > (std::numeric_limits<std::uintptr_t>::max)() - length) {
        win32_error = ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }

    const std::uintptr_t end = start + length;
    std::uintptr_t cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0) {
            win32_error = GetLastError();
            return false;
        }

        const std::uintptr_t region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (region_base > cursor ||
            memory.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - region_base) {
            win32_error = ERROR_INVALID_ADDRESS;
            return false;
        }
        const std::uintptr_t region_end = region_base + memory.RegionSize;
        if (region_end <= cursor || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != module_base ||
            !IsExecutableProtection(memory.Protect)) {
            win32_error = ERROR_INVALID_ADDRESS;
            return false;
        }
        cursor = (std::min)(end, region_end);
    }
    return true;
}

bool GetModulePath(HMODULE module, std::wstring& path, std::uint32_t& error) noexcept {
    try {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            const DWORD length =
                GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                error = GetLastError();
                return false;
            }
            if (length < buffer.size()) {
                buffer.resize(length);
                path = std::move(buffer);
                return true;
            }
            if (buffer.size() > 32768 / 2) {
                error = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            buffer.resize(buffer.size() * 2);
        }
    } catch (const std::bad_alloc&) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

bool ValidateLoadedImage(std::uintptr_t module_base,
                         const PeLayout& layout) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(module_base), &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
        reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != module_base) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!SafeCopyFromImage(reinterpret_cast<const void*>(module_base), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::uint32_t>(dos.e_lfanew) != layout.nt_headers_offset) {
        return false;
    }

    if (!IsContainedRange(layout.nt_headers_offset,
                          sizeof(IMAGE_NT_HEADERS64),
                          layout.size_of_image)) {
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    if (!SafeCopyFromImage(reinterpret_cast<const void*>(module_base + layout.nt_headers_offset),
                           &nt,
                           sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != layout.machine ||
        nt.FileHeader.NumberOfSections != layout.section_count ||
        nt.FileHeader.SizeOfOptionalHeader != layout.optional_header_size ||
        nt.FileHeader.TimeDateStamp != layout.timestamp ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != layout.size_of_image ||
        nt.OptionalHeader.AddressOfEntryPoint != layout.entry_point_rva) {
        return false;
    }

    return true;
}

void StoreResult(Result* destination, const Result& value) noexcept {
    if (destination != nullptr) {
        *destination = value;
    }
}

}  // namespace

bool IsContainedRange(std::uint64_t offset,
                      std::uint64_t length,
                      std::uint64_t extent) noexcept {
    return offset <= extent && length <= extent - offset;
}

RvaMapping ResolveRvaRange(std::uint32_t rva,
                           std::size_t length,
                           const PeLayout& layout,
                           std::span<const PeSection> sections,
                           std::size_t file_size,
                           bool require_executable) noexcept {
    RvaMapping result;
    if (length == 0 || layout.size_of_image == 0 || layout.size_of_headers == 0 ||
        sections.size() != layout.section_count) {
        return result;
    }
    if (!IsContainedRange(rva, length, layout.size_of_image)) {
        result.status = PeStatus::RangeOverflow;
        return result;
    }

    if (rva < layout.size_of_headers) {
        if (require_executable) {
            result.status = PeStatus::RvaNotExecutable;
            return result;
        }
        if (!IsContainedRange(rva, length, layout.size_of_headers) ||
            !IsContainedRange(rva, length, file_size)) {
            result.status = PeStatus::RvaNotFileBacked;
            return result;
        }
        result.status = PeStatus::Ok;
        result.file_offset = rva;
        return result;
    }

    std::size_t match_index = kNoMismatch;
    std::uint64_t match_delta = 0;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const PeSection& section = sections[index];
        const std::uint64_t virtual_span =
            (std::max)(section.virtual_size, section.raw_size);
        if (virtual_span == 0 || rva < section.virtual_address ||
            static_cast<std::uint64_t>(rva) - section.virtual_address >= virtual_span) {
            continue;
        }
        if (match_index != kNoMismatch) {
            result.status = PeStatus::InvalidSectionTable;
            return result;
        }
        match_index = index;
        match_delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
    }

    if (match_index == kNoMismatch) {
        result.status = PeStatus::RvaNotFileBacked;
        return result;
    }

    const PeSection& section = sections[match_index];
    if (require_executable && (section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
        result.status = PeStatus::RvaNotExecutable;
        return result;
    }
    if (!IsContainedRange(match_delta, length, section.raw_size)) {
        result.status = PeStatus::RvaNotFileBacked;
        return result;
    }
    const std::uint64_t file_offset = static_cast<std::uint64_t>(section.raw_offset) + match_delta;
    if (!IsContainedRange(file_offset, length, file_size) ||
        file_offset > (std::numeric_limits<std::size_t>::max)()) {
        result.status = PeStatus::RvaNotFileBacked;
        return result;
    }

    result.status = PeStatus::Ok;
    result.file_offset = static_cast<std::size_t>(file_offset);
    result.section_index = match_index;
    return result;
}

PeStatus ParsePeImage(std::span<const std::uint8_t> file,
                      PeLayout& layout,
                      std::vector<PeSection>& sections) noexcept {
    PeLayout parsed_layout{};
    std::vector<PeSection> parsed_sections;

    IMAGE_DOS_HEADER dos{};
    if (!ReadObject(file, 0, dos)) {
        return PeStatus::Truncated;
    }
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return PeStatus::BadDosSignature;
    }
    if (dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        return PeStatus::InvalidImageLayout;
    }
    const std::size_t nt_offset = static_cast<std::size_t>(dos.e_lfanew);

    DWORD signature = 0;
    IMAGE_FILE_HEADER file_header{};
    if (!ReadObject(file, nt_offset, signature) ||
        !ReadObject(file, nt_offset + sizeof(signature), file_header)) {
        return PeStatus::Truncated;
    }
    if (signature != IMAGE_NT_SIGNATURE) {
        return PeStatus::BadNtSignature;
    }
    if (file_header.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return PeStatus::UnsupportedMachine;
    }
    if (file_header.NumberOfSections == 0 ||
        file_header.NumberOfSections > kMaximumPeSections) {
        return PeStatus::InvalidSectionTable;
    }
    if (file_header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        return PeStatus::UnsupportedOptionalHeader;
    }

    const std::size_t optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    IMAGE_OPTIONAL_HEADER64 optional{};
    if (!ReadObject(file, optional_offset, optional)) {
        return PeStatus::Truncated;
    }
    if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return PeStatus::UnsupportedOptionalHeader;
    }
    if (optional.ImageBase == 0 || optional.SizeOfImage == 0 ||
        optional.SizeOfHeaders == 0 || optional.SectionAlignment == 0 ||
        optional.FileAlignment == 0 || optional.SizeOfHeaders > optional.SizeOfImage ||
        optional.SizeOfHeaders > file.size()) {
        return PeStatus::InvalidImageLayout;
    }

    const std::uint64_t section_table_offset =
        static_cast<std::uint64_t>(optional_offset) + file_header.SizeOfOptionalHeader;
    const std::uint64_t section_table_size =
        static_cast<std::uint64_t>(file_header.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!IsContainedRange(section_table_offset, section_table_size, file.size()) ||
        !IsContainedRange(section_table_offset, section_table_size, optional.SizeOfHeaders)) {
        return PeStatus::InvalidSectionTable;
    }

    try {
        parsed_sections.reserve(file_header.NumberOfSections);
    } catch (const std::bad_alloc&) {
        return PeStatus::AllocationFailed;
    }

    std::uint64_t previous_virtual_end = optional.SizeOfHeaders;
    for (std::size_t index = 0; index < file_header.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER header{};
        if (!ReadObject(file,
                        static_cast<std::size_t>(section_table_offset) +
                            index * sizeof(IMAGE_SECTION_HEADER),
                        header)) {
            return PeStatus::Truncated;
        }

        PeSection section{header.VirtualAddress,
                          header.Misc.VirtualSize,
                          header.PointerToRawData,
                          header.SizeOfRawData,
                          header.Characteristics};
        const std::uint64_t virtual_span =
            (std::max)(section.virtual_size, section.raw_size);
        if (virtual_span != 0) {
            if (section.virtual_address < previous_virtual_end ||
                !IsContainedRange(section.virtual_address,
                                  virtual_span,
                                  optional.SizeOfImage)) {
                return PeStatus::InvalidSectionTable;
            }
            previous_virtual_end =
                static_cast<std::uint64_t>(section.virtual_address) + virtual_span;
        }
        if (section.raw_size != 0 &&
            !IsContainedRange(section.raw_offset, section.raw_size, file.size())) {
            return PeStatus::InvalidSectionTable;
        }
        parsed_sections.push_back(section);
    }

    parsed_layout.preferred_image_base = optional.ImageBase;
    parsed_layout.size_of_image = optional.SizeOfImage;
    parsed_layout.size_of_headers = optional.SizeOfHeaders;
    parsed_layout.timestamp = file_header.TimeDateStamp;
    parsed_layout.entry_point_rva = optional.AddressOfEntryPoint;
    parsed_layout.checksum = optional.CheckSum;
    parsed_layout.nt_headers_offset = static_cast<std::uint32_t>(nt_offset);
    parsed_layout.machine = file_header.Machine;
    parsed_layout.section_count = file_header.NumberOfSections;
    parsed_layout.optional_header_size = file_header.SizeOfOptionalHeader;

    if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        parsed_layout.relocation_rva =
            optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        parsed_layout.relocation_size =
            optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
    }
    if ((parsed_layout.relocation_rva == 0) != (parsed_layout.relocation_size == 0)) {
        return PeStatus::InvalidRelocations;
    }
    if (parsed_layout.relocation_size != 0) {
        const RvaMapping relocation_mapping = ResolveRvaRange(
            parsed_layout.relocation_rva,
            parsed_layout.relocation_size,
            parsed_layout,
            parsed_sections,
            file.size(),
            false);
        if (relocation_mapping.status != PeStatus::Ok) {
            return PeStatus::InvalidRelocations;
        }
    }

    layout = parsed_layout;
    sections = std::move(parsed_sections);
    return PeStatus::Ok;
}

PeStatus BuildExpectedImageBytes(std::span<const std::uint8_t> file,
                                 const PeLayout& layout,
                                 std::span<const PeSection> sections,
                                 std::uint32_t target_rva,
                                 std::uint64_t loaded_image_base,
                                 std::span<std::uint8_t> destination) noexcept {
    if (destination.empty() || loaded_image_base == 0) {
        return PeStatus::InvalidArgument;
    }

    const RvaMapping target_mapping = ResolveRvaRange(
        target_rva, destination.size(), layout, sections, file.size(), false);
    if (target_mapping.status != PeStatus::Ok) {
        return target_mapping.status;
    }
    std::memcpy(destination.data(), file.data() + target_mapping.file_offset, destination.size());

    const bool relocation_below_preferred = loaded_image_base < layout.preferred_image_base;
    const std::uint64_t relocation_magnitude = relocation_below_preferred
                                                   ? layout.preferred_image_base - loaded_image_base
                                                   : loaded_image_base - layout.preferred_image_base;
    if (relocation_magnitude == 0 || layout.relocation_size == 0) {
        return PeStatus::Ok;
    }
    // Keep the relocation arithmetic modulo 64 bits, but compute the delta
    // without unsigned-underflowing when ASLR places an image below its
    // preferred base.
    const std::uint64_t relocation_delta = relocation_below_preferred
                                               ? (0ull - relocation_magnitude)
                                               : relocation_magnitude;

    const RvaMapping directory_mapping = ResolveRvaRange(layout.relocation_rva,
                                                         layout.relocation_size,
                                                         layout,
                                                         sections,
                                                         file.size(),
                                                         false);
    if (directory_mapping.status != PeStatus::Ok) {
        return PeStatus::InvalidRelocations;
    }

    const std::uint64_t target_begin = target_rva;
    const std::uint64_t target_end = target_begin + destination.size();
    std::size_t consumed = 0;
    while (consumed < layout.relocation_size) {
        const std::size_t remaining = layout.relocation_size - consumed;
        if (remaining < sizeof(IMAGE_BASE_RELOCATION)) {
            const auto* padding = file.data() + directory_mapping.file_offset + consumed;
            if (!std::all_of(padding, padding + remaining, [](std::uint8_t byte) {
                    return byte == 0;
                })) {
                return PeStatus::InvalidRelocations;
            }
            break;
        }

        IMAGE_BASE_RELOCATION block{};
        std::memcpy(&block,
                    file.data() + directory_mapping.file_offset + consumed,
                    sizeof(block));
        if (block.VirtualAddress == 0 && block.SizeOfBlock == 0) {
            const auto* padding = file.data() + directory_mapping.file_offset + consumed;
            if (!std::all_of(padding, padding + remaining, [](std::uint8_t byte) {
                    return byte == 0;
                })) {
                return PeStatus::InvalidRelocations;
            }
            break;
        }
        if ((block.VirtualAddress & 0xFFFU) != 0 ||
            block.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(WORD) != 0 ||
            block.SizeOfBlock > remaining) {
            return PeStatus::InvalidRelocations;
        }

        const std::size_t entry_count =
            (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        const auto* entries = reinterpret_cast<const WORD*>(
            file.data() + directory_mapping.file_offset + consumed +
            sizeof(IMAGE_BASE_RELOCATION));
        for (std::size_t index = 0; index < entry_count; ++index) {
            WORD encoded = 0;
            std::memcpy(&encoded, entries + index, sizeof(encoded));
            const std::uint16_t type = encoded >> 12;
            const std::uint16_t page_offset = encoded & 0x0FFFU;
            if (type == IMAGE_REL_BASED_ABSOLUTE) {
                continue;
            }

            std::size_t relocation_width = 0;
            if (type == IMAGE_REL_BASED_DIR64) {
                relocation_width = sizeof(std::uint64_t);
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                relocation_width = sizeof(std::uint32_t);
            } else {
                return PeStatus::UnsupportedRelocation;
            }

            const std::uint64_t relocation_rva =
                static_cast<std::uint64_t>(block.VirtualAddress) + page_offset;
            if (!IsContainedRange(relocation_rva, relocation_width, layout.size_of_image)) {
                return PeStatus::InvalidRelocations;
            }
            const std::uint64_t relocation_end = relocation_rva + relocation_width;
            if (relocation_end <= target_begin || relocation_rva >= target_end) {
                continue;
            }

            const RvaMapping value_mapping = ResolveRvaRange(
                static_cast<std::uint32_t>(relocation_rva),
                relocation_width,
                layout,
                sections,
                file.size(),
                false);
            if (value_mapping.status != PeStatus::Ok) {
                return PeStatus::InvalidRelocations;
            }

            std::array<std::uint8_t, sizeof(std::uint64_t)> relocated_bytes{};
            if (relocation_width == sizeof(std::uint64_t)) {
                std::uint64_t value = 0;
                std::memcpy(&value, file.data() + value_mapping.file_offset, sizeof(value));
                value += relocation_delta;
                std::memcpy(relocated_bytes.data(), &value, sizeof(value));
            } else {
                std::uint32_t value = 0;
                std::memcpy(&value, file.data() + value_mapping.file_offset, sizeof(value));
                value += static_cast<std::uint32_t>(relocation_delta);
                std::memcpy(relocated_bytes.data(), &value, sizeof(value));
            }

            const std::uint64_t overlap_begin = (std::max)(target_begin, relocation_rva);
            const std::uint64_t overlap_end = (std::min)(target_end, relocation_end);
            for (std::uint64_t byte_rva = overlap_begin; byte_rva < overlap_end; ++byte_rva) {
                destination[static_cast<std::size_t>(byte_rva - target_begin)] =
                    relocated_bytes[static_cast<std::size_t>(byte_rva - relocation_rva)];
            }
        }

        consumed += block.SizeOfBlock;
    }

    return PeStatus::Ok;
}

const char* PeStatusName(PeStatus status) noexcept {
    switch (status) {
        case PeStatus::Ok:
            return "ok";
        case PeStatus::InvalidArgument:
            return "invalid_argument";
        case PeStatus::RangeOverflow:
            return "range_overflow";
        case PeStatus::Truncated:
            return "truncated";
        case PeStatus::BadDosSignature:
            return "bad_dos_signature";
        case PeStatus::BadNtSignature:
            return "bad_nt_signature";
        case PeStatus::UnsupportedMachine:
            return "unsupported_machine";
        case PeStatus::UnsupportedOptionalHeader:
            return "unsupported_optional_header";
        case PeStatus::InvalidImageLayout:
            return "invalid_image_layout";
        case PeStatus::InvalidSectionTable:
            return "invalid_section_table";
        case PeStatus::RvaNotFileBacked:
            return "rva_not_file_backed";
        case PeStatus::RvaNotExecutable:
            return "rva_not_executable";
        case PeStatus::InvalidRelocations:
            return "invalid_relocations";
        case PeStatus::UnsupportedRelocation:
            return "unsupported_relocation";
        case PeStatus::AllocationFailed:
            return "allocation_failed";
    }
    return "unknown";
}

const char* StatusName(Status status) noexcept {
    switch (status) {
        case Status::Verified:
            return "verified";
        case Status::NotInitialized:
            return "not_initialized";
        case Status::InvalidArgument:
            return "invalid_argument";
        case Status::ImagePathUnavailable:
            return "image_path_unavailable";
        case Status::FileOpenFailed:
            return "file_open_failed";
        case Status::FileSizeInvalid:
            return "file_size_invalid";
        case Status::FileMappingFailed:
            return "file_mapping_failed";
        case Status::InvalidPeImage:
            return "invalid_pe_image";
        case Status::LoadedImageMismatch:
            return "loaded_image_mismatch";
        case Status::TargetOutsideImage:
            return "target_outside_image";
        case Status::TargetNotExecutable:
            return "target_not_executable";
        case Status::TargetMemoryInvalid:
            return "target_memory_invalid";
        case Status::TargetReadFailed:
            return "target_read_failed";
        case Status::Modified:
            return "modified";
        case Status::AllocationFailed:
            return "allocation_failed";
    }
    return "unknown";
}

struct Guard::State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const std::uint8_t* view = nullptr;
    std::size_t file_size = 0;
    std::uintptr_t module_base = 0;
    PeLayout layout{};
    std::vector<PeSection> sections;

    ~State() {
        if (view != nullptr) {
            UnmapViewOfFile(view);
        }
        if (mapping != nullptr) {
            CloseHandle(mapping);
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
};

Guard::~Guard() {
    Reset();
}

bool Guard::Initialize(void* module_base, Result* result) noexcept {
    Reset();

    Result current;
    std::unique_ptr<State> pending(new (std::nothrow) State());
    if (!pending) {
        current.status = Status::AllocationFailed;
        StoreResult(result, current);
        return false;
    }

    HMODULE module = reinterpret_cast<HMODULE>(module_base);
    if (module == nullptr) {
        module = GetModuleHandleW(nullptr);
    }
    if (module == nullptr) {
        current.status = Status::ImagePathUnavailable;
        current.win32_error = GetLastError();
        StoreResult(result, current);
        return false;
    }
    pending->module_base = reinterpret_cast<std::uintptr_t>(module);

    std::wstring path;
    if (!GetModulePath(module, path, current.win32_error)) {
        current.status = current.win32_error == ERROR_NOT_ENOUGH_MEMORY
                             ? Status::AllocationFailed
                             : Status::ImagePathUnavailable;
        StoreResult(result, current);
        return false;
    }

    pending->file = CreateFileW(path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (pending->file == INVALID_HANDLE_VALUE) {
        current.status = Status::FileOpenFailed;
        current.win32_error = GetLastError();
        StoreResult(result, current);
        return false;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(pending->file, &file_size) || file_size.QuadPart <= 0 ||
        static_cast<unsigned long long>(file_size.QuadPart) >
            (std::numeric_limits<std::size_t>::max)()) {
        current.status = Status::FileSizeInvalid;
        current.win32_error = GetLastError();
        StoreResult(result, current);
        return false;
    }
    pending->file_size = static_cast<std::size_t>(file_size.QuadPart);

    pending->mapping = CreateFileMappingW(pending->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (pending->mapping == nullptr) {
        current.status = Status::FileMappingFailed;
        current.win32_error = GetLastError();
        StoreResult(result, current);
        return false;
    }
    pending->view = static_cast<const std::uint8_t*>(
        MapViewOfFile(pending->mapping, FILE_MAP_READ, 0, 0, 0));
    if (pending->view == nullptr) {
        current.status = Status::FileMappingFailed;
        current.win32_error = GetLastError();
        StoreResult(result, current);
        return false;
    }

    current.pe_status = ParsePeImage(
        std::span<const std::uint8_t>(pending->view, pending->file_size),
        pending->layout,
        pending->sections);
    if (current.pe_status != PeStatus::Ok) {
        current.status = current.pe_status == PeStatus::AllocationFailed
                             ? Status::AllocationFailed
                             : Status::InvalidPeImage;
        StoreResult(result, current);
        return false;
    }
    if (!ValidateLoadedImage(pending->module_base, pending->layout)) {
        current.status = Status::LoadedImageMismatch;
        StoreResult(result, current);
        return false;
    }

    state_ = pending.release();
    current.status = Status::Verified;
    StoreResult(result, current);
    return true;
}

bool Guard::InitializeForAddress(const void* target, Result* result) noexcept {
    Reset();

    Result current;
    if (target == nullptr) {
        current.status = Status::InvalidArgument;
        StoreResult(result, current);
        return false;
    }

    HMODULE module = nullptr;
    // FROM_ADDRESS treats the second parameter as an address rather than a
    // module name.  Do not acquire an extra loader reference: the caller's
    // live target/object owns the module for this short transaction, and a
    // failed SMAA retry must not leak one reference per frame.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(target),
                            &module) ||
        module == nullptr) {
        current.status = Status::ImagePathUnavailable;
        current.win32_error = GetLastError();
        StoreResult(result, current);
        return false;
    }

    return Initialize(reinterpret_cast<void*>(module), result);
}

Result Guard::Verify(const void* target, std::size_t byte_count) const noexcept {
    Result result;
    result.byte_count = byte_count;
    if (state_ == nullptr) {
        return result;
    }
    if (target == nullptr || byte_count == 0 || byte_count > kMaximumPrologueBytes) {
        result.status = Status::InvalidArgument;
        return result;
    }

    const std::uintptr_t target_address = reinterpret_cast<std::uintptr_t>(target);
    if (target_address < state_->module_base) {
        result.status = Status::TargetOutsideImage;
        return result;
    }
    const std::uint64_t rva =
        static_cast<std::uint64_t>(target_address - state_->module_base);
    if (rva > (std::numeric_limits<std::uint32_t>::max)() ||
        !IsContainedRange(rva, byte_count, state_->layout.size_of_image)) {
        result.status = Status::TargetOutsideImage;
        return result;
    }
    result.target_rva = static_cast<std::uint32_t>(rva);

    const RvaMapping mapping = ResolveRvaRange(result.target_rva,
                                               byte_count,
                                               state_->layout,
                                               state_->sections,
                                               state_->file_size,
                                               true);
    result.pe_status = mapping.status;
    if (mapping.status != PeStatus::Ok) {
        result.status = mapping.status == PeStatus::RvaNotExecutable ||
                                mapping.status == PeStatus::RvaNotFileBacked
                            ? Status::TargetNotExecutable
                            : Status::InvalidPeImage;
        return result;
    }

    if (!ValidateExecutableMemoryRange(target_address,
                                       byte_count,
                                       state_->module_base,
                                       result.win32_error)) {
        result.status = Status::TargetMemoryInvalid;
        return result;
    }

    std::array<std::uint8_t, kMaximumPrologueBytes> expected{};
    result.pe_status = BuildExpectedImageBytes(
        std::span<const std::uint8_t>(state_->view, state_->file_size),
        state_->layout,
        state_->sections,
        result.target_rva,
        state_->module_base,
        std::span<std::uint8_t>(expected.data(), byte_count));
    if (result.pe_status != PeStatus::Ok) {
        result.status = Status::InvalidPeImage;
        return result;
    }

    std::array<std::uint8_t, kMaximumPrologueBytes> live{};
    if (!SafeCopyFromImage(target, live.data(), byte_count)) {
        result.status = Status::TargetReadFailed;
        return result;
    }
    for (std::size_t index = 0; index < byte_count; ++index) {
        if (live[index] != expected[index]) {
            result.status = Status::Modified;
            result.mismatch_offset = index;
            return result;
        }
    }

    result.status = Status::Verified;
    return result;
}

void Guard::Reset() noexcept {
    delete state_;
    state_ = nullptr;
}

}  // namespace spatch::hook_guard
