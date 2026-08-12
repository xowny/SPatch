#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "BigFileIndexPolicy.h"

namespace spatch::chunk_stream {

inline constexpr std::size_t kHeaderSize = 0x10;
inline constexpr std::size_t kAlignment = 4;

struct Header {
    std::uint32_t id = 0;
    std::int32_t chunk_size = 0;
    std::int32_t data_size = 0;
    std::uint32_t data_offset = 0;
};

static_assert(sizeof(Header) == kHeaderSize);

enum class Error : std::uint8_t {
    None,
    NullBuffer,
    TruncatedHeader,
    NegativeChunkSize,
    ChunkExceedsBuffer,
    InvalidBigFileIndex,
    BufferAccessException,
};

struct Result {
    Error error = Error::None;
    std::size_t chunk_count = 0;
    std::size_t offset = 0;
    Header header{};
    big_file_index::Result big_file_result{};

    [[nodiscard]] bool valid() const noexcept { return error == Error::None; }
};

inline Result Validate(const void* buffer, std::size_t size) noexcept {
    if (size == 0) {
        return {};
    }
    if (buffer == nullptr) {
        return {Error::NullBuffer};
    }

    const auto* const bytes = static_cast<const std::byte*>(buffer);
    std::size_t offset = 0;
    std::size_t chunk_count = 0;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        if (remaining < kHeaderSize) {
            return {Error::TruncatedHeader, chunk_count, offset};
        }

        Header header{};
        std::memcpy(&header, bytes + offset, sizeof(header));
        if (header.chunk_size < 0) {
            return {Error::NegativeChunkSize, chunk_count, offset, header};
        }
        const std::size_t chunk_size = static_cast<std::size_t>(header.chunk_size);
        constexpr std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
        if (chunk_size > maximum - (kAlignment - 1)) {
            return {Error::ChunkExceedsBuffer, chunk_count, offset, header};
        }
        const std::size_t aligned_chunk_size =
            (chunk_size + (kAlignment - 1)) & ~(kAlignment - 1);
        if (aligned_chunk_size > remaining - kHeaderSize) {
            return {Error::ChunkExceedsBuffer, chunk_count, offset, header};
        }

        if (header.id == big_file_index::kChunkId) {
            const big_file_index::Result big_file_result =
                big_file_index::Validate(bytes + offset + kHeaderSize,
                                         chunk_size,
                                         header.data_size,
                                         header.data_offset);
            if (!big_file_result.valid()) {
                return {Error::InvalidBigFileIndex,
                        chunk_count,
                        offset,
                        header,
                        big_file_result};
            }
        }

        offset += kHeaderSize + aligned_chunk_size;
        ++chunk_count;
    }
    return {Error::None, chunk_count, offset};
}

inline const char* ErrorName(Error error) noexcept {
    switch (error) {
    case Error::None:
        return "none";
    case Error::NullBuffer:
        return "null_buffer";
    case Error::TruncatedHeader:
        return "truncated_header";
    case Error::NegativeChunkSize:
        return "negative_chunk_size";
    case Error::ChunkExceedsBuffer:
        return "chunk_exceeds_buffer";
    case Error::InvalidBigFileIndex:
        return "invalid_big_file_index";
    case Error::BufferAccessException:
        return "buffer_access_exception";
    default:
        return "unknown";
    }
}

}  // namespace spatch::chunk_stream
