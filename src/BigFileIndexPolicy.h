#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace spatch::big_file_index {

inline constexpr std::uint32_t kChunkId = 0x2C5C40A8;
inline constexpr std::uint32_t kTypeId = 0x2AE784F9;
inline constexpr std::size_t kTypeIdOffset = 0x30;
inline constexpr std::size_t kEntryCountOffset = 0x60;
inline constexpr std::size_t kEntriesRelativeOffset = 0x68;
inline constexpr std::size_t kSerializedMetadataSize = 0xA8;
inline constexpr std::size_t kEntrySize = 0x18;

enum class Error : std::uint8_t {
    None,
    NullPayload,
    TruncatedMetadata,
    ChunkAndDataSizeMismatch,
    UnexpectedDataOffset,
    TypeMismatch,
    EntryCountExceedsSignedRange,
    EntriesPointerBeforePayload,
    EntriesPointerInsideMetadata,
    EntriesPointerAfterPayload,
    EntryArrayExceedsPayload,
    EntriesNotSorted,
};

struct Result {
    Error error = Error::None;
    std::uint32_t entry_count = 0;
    std::size_t entries_offset = 0;
    std::uint32_t offending_index = 0;

    [[nodiscard]] bool valid() const noexcept { return error == Error::None; }
};

template <typename T>
inline T Load(const std::byte* payload, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, payload + offset, sizeof(value));
    return value;
}

inline Result Validate(const void* payload_buffer,
                       std::size_t chunk_size,
                       std::int32_t data_size,
                       std::uint32_t data_offset) noexcept {
    if (payload_buffer == nullptr) {
        return {Error::NullPayload};
    }
    if (chunk_size < kSerializedMetadataSize) {
        return {Error::TruncatedMetadata};
    }
    if (data_size < 0 || static_cast<std::size_t>(data_size) != chunk_size) {
        return {Error::ChunkAndDataSizeMismatch};
    }
    if (data_offset != 0) {
        return {Error::UnexpectedDataOffset};
    }

    const auto* const payload = static_cast<const std::byte*>(payload_buffer);
    if (Load<std::uint32_t>(payload, kTypeIdOffset) != kTypeId) {
        return {Error::TypeMismatch};
    }

    const std::uint32_t entry_count =
        Load<std::uint32_t>(payload, kEntryCountOffset);
    if (entry_count > static_cast<std::uint32_t>(
                          (std::numeric_limits<std::int32_t>::max)())) {
        return {Error::EntryCountExceedsSignedRange, entry_count};
    }
    if (entry_count == 0) {
        return {Error::None, 0, kSerializedMetadataSize};
    }

    const std::int64_t relative_offset =
        Load<std::int64_t>(payload, kEntriesRelativeOffset);
    if (relative_offset < -static_cast<std::int64_t>(kEntriesRelativeOffset)) {
        return {Error::EntriesPointerBeforePayload, entry_count};
    }
    if (relative_offset > (std::numeric_limits<std::int64_t>::max)() -
                              static_cast<std::int64_t>(kEntriesRelativeOffset)) {
        return {Error::EntriesPointerAfterPayload, entry_count};
    }
    const std::uint64_t entries_offset_u64 =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(kEntriesRelativeOffset) +
                                   relative_offset);
    if (entries_offset_u64 < kSerializedMetadataSize) {
        return {Error::EntriesPointerInsideMetadata, entry_count,
                static_cast<std::size_t>(entries_offset_u64)};
    }
    if (entries_offset_u64 > chunk_size) {
        return {Error::EntriesPointerAfterPayload, entry_count,
                static_cast<std::size_t>(entries_offset_u64)};
    }

    const std::size_t entries_offset = static_cast<std::size_t>(entries_offset_u64);
    if (entry_count > (chunk_size - entries_offset) / kEntrySize) {
        return {Error::EntryArrayExceedsPayload, entry_count, entries_offset};
    }

    std::uint32_t previous_uid = Load<std::uint32_t>(payload, entries_offset);
    for (std::uint32_t index = 1; index < entry_count; ++index) {
        const std::uint32_t uid =
            Load<std::uint32_t>(payload, entries_offset +
                                             static_cast<std::size_t>(index) * kEntrySize);
        if (uid < previous_uid) {
            return {Error::EntriesNotSorted, entry_count, entries_offset, index};
        }
        previous_uid = uid;
    }
    return {Error::None, entry_count, entries_offset};
}

inline const char* ErrorName(Error error) noexcept {
    switch (error) {
    case Error::None:
        return "none";
    case Error::NullPayload:
        return "null_payload";
    case Error::TruncatedMetadata:
        return "truncated_metadata";
    case Error::ChunkAndDataSizeMismatch:
        return "chunk_and_data_size_mismatch";
    case Error::UnexpectedDataOffset:
        return "unexpected_data_offset";
    case Error::TypeMismatch:
        return "type_mismatch";
    case Error::EntryCountExceedsSignedRange:
        return "entry_count_exceeds_signed_range";
    case Error::EntriesPointerBeforePayload:
        return "entries_pointer_before_payload";
    case Error::EntriesPointerInsideMetadata:
        return "entries_pointer_inside_metadata";
    case Error::EntriesPointerAfterPayload:
        return "entries_pointer_after_payload";
    case Error::EntryArrayExceedsPayload:
        return "entry_array_exceeds_payload";
    case Error::EntriesNotSorted:
        return "entries_not_sorted";
    default:
        return "unknown";
    }
}

}  // namespace spatch::big_file_index
