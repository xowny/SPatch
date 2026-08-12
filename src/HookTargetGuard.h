#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace spatch::hook_guard {

// Hook installation compares a short, caller-selected function prologue.  The
// bound keeps accidental whole-image comparisons out of the bootstrap path
// while leaving room for long, instruction-aligned signatures.
inline constexpr std::size_t kMaximumPrologueBytes = 64;
// The bundled x64 MinHook decoder may inspect/relocate the instruction that
// follows the minimum absolute jump span.  With a maximum x86-64 instruction
// length of 15 bytes, 32 bytes cover that complete decoder window (and the
// bytes a competing detour could have changed) without requiring a second
// disassembler in the bootstrap path.
inline constexpr std::size_t kMinHookTargetVerificationBytes = 32;
inline constexpr std::size_t kNoMismatch = (std::numeric_limits<std::size_t>::max)();

enum class PeStatus {
    Ok,
    InvalidArgument,
    RangeOverflow,
    Truncated,
    BadDosSignature,
    BadNtSignature,
    UnsupportedMachine,
    UnsupportedOptionalHeader,
    InvalidImageLayout,
    InvalidSectionTable,
    RvaNotFileBacked,
    RvaNotExecutable,
    InvalidRelocations,
    UnsupportedRelocation,
    AllocationFailed,
};

struct PeSection {
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
};

struct PeLayout {
    std::uint64_t preferred_image_base = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t size_of_headers = 0;
    std::uint32_t relocation_rva = 0;
    std::uint32_t relocation_size = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t entry_point_rva = 0;
    std::uint32_t checksum = 0;
    std::uint32_t nt_headers_offset = 0;
    std::uint16_t machine = 0;
    std::uint16_t section_count = 0;
    std::uint16_t optional_header_size = 0;
};

struct RvaMapping {
    PeStatus status = PeStatus::InvalidArgument;
    std::size_t file_offset = 0;
    std::size_t section_index = kNoMismatch;
};

// Pure helpers used by the runtime guard and synthetic-PE tests.
[[nodiscard]] bool IsContainedRange(std::uint64_t offset,
                                    std::uint64_t length,
                                    std::uint64_t extent) noexcept;
[[nodiscard]] RvaMapping ResolveRvaRange(std::uint32_t rva,
                                         std::size_t length,
                                         const PeLayout& layout,
                                         std::span<const PeSection> sections,
                                         std::size_t file_size,
                                         bool require_executable) noexcept;
[[nodiscard]] PeStatus ParsePeImage(std::span<const std::uint8_t> file,
                                    PeLayout& layout,
                                    std::vector<PeSection>& sections) noexcept;
[[nodiscard]] PeStatus BuildExpectedImageBytes(
    std::span<const std::uint8_t> file,
    const PeLayout& layout,
    std::span<const PeSection> sections,
    std::uint32_t target_rva,
    std::uint64_t loaded_image_base,
    std::span<std::uint8_t> destination) noexcept;
[[nodiscard]] const char* PeStatusName(PeStatus status) noexcept;

enum class Status {
    Verified,
    NotInitialized,
    InvalidArgument,
    ImagePathUnavailable,
    FileOpenFailed,
    FileSizeInvalid,
    FileMappingFailed,
    InvalidPeImage,
    LoadedImageMismatch,
    TargetOutsideImage,
    TargetNotExecutable,
    TargetMemoryInvalid,
    TargetReadFailed,
    Modified,
    AllocationFailed,
};

struct Result {
    Status status = Status::NotInitialized;
    PeStatus pe_status = PeStatus::Ok;
    std::uint32_t target_rva = 0;
    std::size_t byte_count = 0;
    std::size_t mismatch_offset = kNoMismatch;
    std::uint32_t win32_error = 0;

    [[nodiscard]] bool verified() const noexcept { return status == Status::Verified; }
};

[[nodiscard]] const char* StatusName(Status status) noexcept;

// Single-owner bootstrap helper.  Initialize maps the exact executable backing
// the supplied loaded module (the process executable when module_base is null).
// Keep the object alive while every target is checked immediately before its
// MH_CreateHook call, then Reset it after the hook transaction.
class Guard {
public:
    Guard() noexcept = default;
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&&) = delete;
    Guard& operator=(Guard&&) = delete;

    [[nodiscard]] bool Initialize(void* module_base = nullptr,
                                  Result* result = nullptr) noexcept;
    // Resolve the PE image that owns an arbitrary executable address (for
    // example a COM vtable method in dxgi.dll) and initialize the same
    // pristine-image snapshot against that module.  The lookup deliberately
    // uses UNCHANGED_REFCOUNT: callers must keep the owning object/module
    // alive for the duration of the guarded hook transaction, while the guard
    // itself must not leak a loader reference on every retry.
    [[nodiscard]] bool InitializeForAddress(const void* target,
                                             Result* result = nullptr) noexcept;
    [[nodiscard]] Result Verify(const void* target, std::size_t byte_count) const noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool initialized() const noexcept { return state_ != nullptr; }

private:
    struct State;
    State* state_ = nullptr;
};

}  // namespace spatch::hook_guard
