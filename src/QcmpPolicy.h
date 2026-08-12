#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace spatch::qcmp {

inline constexpr std::uint32_t kCanonicalMagic = 0x51434D50;
inline constexpr std::uint32_t kSwappedMagic = 0x504D4351;
inline constexpr std::uint64_t kFailure = (std::numeric_limits<std::uint64_t>::max)();
inline constexpr std::size_t kHeaderSize = 0x40;
inline constexpr std::uint16_t kLzType = 1;
inline constexpr std::uint16_t kFormatVersion = 1;

enum class Error : std::uint8_t {
    None,
    NullBuffer,
    NullDestination,
    InvalidSize,
    SourceSmallerThanHeader,
    InvalidMagic,
    InvalidType,
    InvalidVersion,
    DataOffsetBeforeHeader,
    UnexpectedDataOffset,
    StreamEndBeforeData,
    StreamEndAfterSource,
    CompressedSizeMismatch,
    DeclaredOutputExceedsDestination,
    LiteralCrossesStream,
    TokenCrossesStream,
    UninitializedPattern,
    BackreferenceBeforeOutput,
    OutputExceedsDestination,
    OutputSizeMismatch,
    BufferAccessException,
};

struct Result {
    Error error = Error::None;
    std::uint64_t output_size = 0;
    std::uint32_t data_offset = 0;
    std::uint64_t stream_end = 0;
    std::uint64_t declared_output_size = 0;

    [[nodiscard]] bool valid() const noexcept { return error == Error::None; }
};

struct Pattern {
    std::uint32_t distance = 0;
    std::uint32_t length = 0;
    bool initialized = false;
};

namespace detail {

inline std::uint32_t ByteSwap32(std::uint32_t value) noexcept {
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

inline std::uint16_t ByteSwap16(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

inline std::uint64_t ByteSwap64(std::uint64_t value) noexcept {
    return (static_cast<std::uint64_t>(ByteSwap32(static_cast<std::uint32_t>(value))) << 32) |
           ByteSwap32(static_cast<std::uint32_t>(value >> 32));
}

template <typename T>
inline T Load(const std::byte* source, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, source + offset, sizeof(value));
    return value;
}

inline Result Process(const void* source_buffer,
                      std::uint64_t source_size,
                      void* destination_buffer,
                      std::uint64_t destination_capacity,
                      bool write_output) noexcept {
    if (source_buffer == nullptr) {
        return {Error::NullBuffer};
    }
    if (source_size == kFailure || destination_capacity == kFailure ||
        source_size > (std::numeric_limits<std::size_t>::max)() ||
        destination_capacity > (std::numeric_limits<std::size_t>::max)()) {
        return {Error::InvalidSize};
    }
    if (source_size < kHeaderSize) {
        return {Error::SourceSmallerThanHeader};
    }

    const auto* const source = static_cast<const std::byte*>(source_buffer);
    auto* const destination = static_cast<std::byte*>(destination_buffer);
    const std::uint32_t magic = Load<std::uint32_t>(source, 0);
    if (magic != kCanonicalMagic && magic != kSwappedMagic) {
        return {Error::InvalidMagic};
    }
    const bool swapped = magic == kSwappedMagic;
    std::uint16_t type = Load<std::uint16_t>(source, 0x04);
    std::uint16_t version = Load<std::uint16_t>(source, 0x06);
    std::uint32_t data_offset = Load<std::uint32_t>(source, 0x08);
    std::uint64_t stream_end = Load<std::uint64_t>(source, 0x10);
    std::uint64_t declared_output_size = Load<std::uint64_t>(source, 0x18);
    if (swapped) {
        type = ByteSwap16(type);
        version = ByteSwap16(version);
        data_offset = ByteSwap32(data_offset);
        stream_end = ByteSwap64(stream_end);
        declared_output_size = ByteSwap64(declared_output_size);
    }
    if (type != kLzType) {
        return {Error::InvalidType, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (version != kFormatVersion) {
        return {Error::InvalidVersion, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (data_offset < kHeaderSize) {
        return {Error::DataOffsetBeforeHeader, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (data_offset != kHeaderSize) {
        return {Error::UnexpectedDataOffset, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (stream_end < data_offset) {
        return {Error::StreamEndBeforeData, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (stream_end > source_size) {
        return {Error::StreamEndAfterSource, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (stream_end != source_size) {
        return {Error::CompressedSizeMismatch, 0, data_offset, stream_end,
                declared_output_size};
    }
    if (declared_output_size > destination_capacity) {
        return {Error::DeclaredOutputExceedsDestination, 0, data_offset,
                stream_end, declared_output_size};
    }

    std::array<Pattern, 32> patterns{};
    std::size_t pattern_index = 0;
    std::uint64_t cursor = data_offset;
    std::uint64_t output_size = 0;
    while (cursor < stream_end) {
        const auto token = std::to_integer<std::uint8_t>(source[cursor++]);
        if (token < 0x20) {
            const std::uint64_t literal_count = static_cast<std::uint64_t>(token) + 1;
            if (literal_count > stream_end - cursor) {
                return {Error::LiteralCrossesStream, output_size, data_offset,
                        stream_end, declared_output_size};
            }
            if (output_size > destination_capacity ||
                literal_count > destination_capacity - output_size) {
                return {Error::OutputExceedsDestination, output_size,
                        data_offset, stream_end, declared_output_size};
            }
            if (write_output) {
                std::memcpy(destination + static_cast<std::size_t>(output_size),
                            source + static_cast<std::size_t>(cursor),
                            static_cast<std::size_t>(literal_count));
            }
            cursor += literal_count;
            output_size += literal_count;
            continue;
        }

        const std::uint32_t group = token >> 5;
        std::uint32_t distance = 0;
        std::uint32_t length = 0;
        if (group == 1) {
            const Pattern& pattern = patterns[token & 0x1F];
            if (!pattern.initialized) {
                return {Error::UninitializedPattern, output_size, data_offset,
                        stream_end, declared_output_size};
            }
            distance = pattern.distance;
            length = pattern.length;
        } else {
            if (cursor >= stream_end) {
                return {Error::TokenCrossesStream, output_size, data_offset,
                        stream_end, declared_output_size};
            }
            distance = (static_cast<std::uint32_t>(token & 0x1F) << 8) |
                       std::to_integer<std::uint8_t>(source[cursor++]);
            if (group == 7) {
                if (cursor >= stream_end) {
                    return {Error::TokenCrossesStream, output_size,
                            data_offset, stream_end, declared_output_size};
                }
                length = static_cast<std::uint32_t>(
                             std::to_integer<std::uint8_t>(source[cursor++])) +
                         1;
            } else {
                length = group + 1;
            }
            patterns[pattern_index] = Pattern{distance, length, true};
            pattern_index = (pattern_index + 1) & 0x1F;
        }

        if (distance == 0 || distance > output_size) {
            return {Error::BackreferenceBeforeOutput, output_size, data_offset,
                    stream_end, declared_output_size};
        }
        if (output_size > destination_capacity ||
            length > destination_capacity - output_size) {
            return {Error::OutputExceedsDestination, output_size, data_offset,
                    stream_end, declared_output_size};
        }
        if (write_output) {
            for (std::uint32_t index = 0; index < length; ++index) {
                const std::uint64_t destination_index = output_size + index;
                destination[static_cast<std::size_t>(destination_index)] =
                    destination[static_cast<std::size_t>(destination_index - distance)];
            }
        }
        output_size += length;
    }

    if (output_size != declared_output_size) {
        return {Error::OutputSizeMismatch, output_size, data_offset, stream_end,
                declared_output_size};
    }
    return {Error::None, output_size, data_offset, stream_end,
            declared_output_size};
}

}  // namespace detail

inline std::uint32_t ByteSwap32(std::uint32_t value) noexcept {
    return detail::ByteSwap32(value);
}

inline std::uint16_t ByteSwap16(std::uint16_t value) noexcept {
    return detail::ByteSwap16(value);
}

inline std::uint64_t ByteSwap64(std::uint64_t value) noexcept {
    return detail::ByteSwap64(value);
}

inline Result Validate(const void* source_buffer,
                       std::uint64_t source_size,
                       std::uint64_t destination_capacity) noexcept {
    return detail::Process(source_buffer, source_size, nullptr,
                           destination_capacity, false);
}

inline Result Decode(const void* source_buffer,
                     std::uint64_t source_size,
                     void* destination_buffer,
                     std::uint64_t destination_capacity) noexcept {
    if (destination_buffer == nullptr) {
        return {Error::NullDestination};
    }
    return detail::Process(source_buffer, source_size, destination_buffer,
                           destination_capacity, true);
}

inline const char* ErrorName(Error error) noexcept {
    switch (error) {
        case Error::None:
            return "none";
        case Error::NullBuffer:
            return "null_buffer";
        case Error::NullDestination:
            return "null_destination";
        case Error::InvalidSize:
            return "invalid_size";
        case Error::SourceSmallerThanHeader:
            return "source_smaller_than_header";
        case Error::InvalidMagic:
            return "invalid_magic";
        case Error::InvalidType:
            return "invalid_type";
        case Error::InvalidVersion:
            return "invalid_version";
        case Error::DataOffsetBeforeHeader:
            return "data_offset_before_header";
        case Error::UnexpectedDataOffset:
            return "unexpected_data_offset";
        case Error::StreamEndBeforeData:
            return "stream_end_before_data";
        case Error::StreamEndAfterSource:
            return "stream_end_after_source";
        case Error::CompressedSizeMismatch:
            return "compressed_size_mismatch";
        case Error::DeclaredOutputExceedsDestination:
            return "declared_output_exceeds_destination";
        case Error::LiteralCrossesStream:
            return "literal_crosses_stream";
        case Error::TokenCrossesStream:
            return "token_crosses_stream";
        case Error::UninitializedPattern:
            return "uninitialized_pattern";
        case Error::BackreferenceBeforeOutput:
            return "backreference_before_output";
        case Error::OutputExceedsDestination:
            return "output_exceeds_destination";
        case Error::OutputSizeMismatch:
            return "output_size_mismatch";
        case Error::BufferAccessException:
            return "buffer_access_exception";
        default:
            return "unknown";
    }
}

}  // namespace spatch::qcmp
