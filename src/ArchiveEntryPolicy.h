#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace spatch::archive_io {

inline constexpr std::size_t kInventoryResourceUidOffset = 0x18;
inline constexpr std::size_t kStreamFileKindOffset = 0x18;
inline constexpr std::size_t kStreamFileInventoryOffset = 0x20;
inline constexpr std::size_t kStreamFileEntryOffset = 0x28;
inline constexpr std::size_t kStreamFileQFileOffset = 0x38;
inline constexpr std::uint32_t kArchiveStreamFileKind = 1;
inline constexpr std::uint32_t kCompressedReadPrefixMask = 0xFFF;

struct EntryDescriptor {
    std::uint32_t uid = 0;
    std::uint32_t offset_divided_by_four = 0;
    std::uint32_t load_offset = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t allocation_extra = 0;
    std::uint32_t uncompressed_size = 0;
};

static_assert(sizeof(EntryDescriptor) == 0x18);

struct ReadRange {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t end = 0;
};

inline bool TryComputeReadRange(const EntryDescriptor& entry, ReadRange& range) noexcept {
    constexpr std::uint64_t kMaximum = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t offset = static_cast<std::uint64_t>(entry.offset_divided_by_four) << 2;

    std::uint64_t read_size = entry.uncompressed_size;
    if (entry.compressed_size != 0 && entry.compressed_size != entry.uncompressed_size) {
        read_size = entry.compressed_size;
        const std::uint64_t prefix = entry.load_offset & kCompressedReadPrefixMask;
        if (read_size > kMaximum - prefix) {
            return false;
        }
        read_size += prefix;
    }

    if (offset > kMaximum - read_size) {
        return false;
    }
    range = ReadRange{offset, read_size, offset + read_size};
    return true;
}

inline bool IsWithinArchive(const EntryDescriptor& entry, std::uint64_t archive_size,
                            ReadRange* range_out = nullptr) noexcept {
    ReadRange range{};
    if (!TryComputeReadRange(entry, range)) {
        return false;
    }
    if (range_out != nullptr) {
        *range_out = range;
    }
    return range.offset <= archive_size && range.size <= archive_size - range.offset;
}

}  // namespace spatch::archive_io
