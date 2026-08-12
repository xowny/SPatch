#include "SPatchSdaoDxbc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using spatch::graphics::sdao::dxbc::InstrumentationStatus;

constexpr std::uint32_t kDxbc = 0x43425844;
constexpr std::uint32_t kShdr = 0x52444853;
constexpr std::uint32_t kShex = 0x58454853;
constexpr std::uint32_t kRdef = 0x46454452;
constexpr std::uint32_t kIsgn = 0x4e475349;
constexpr std::uint32_t kOsgn = 0x4e47534f;
constexpr std::uint32_t kStat = 0x54415453;
constexpr std::uint32_t kAdd = 0;
constexpr std::uint32_t kBreak = 2;
constexpr std::uint32_t kBreakc = 3;
constexpr std::uint32_t kCall = 4;
constexpr std::uint32_t kCallc = 5;
constexpr std::uint32_t kCase = 6;
constexpr std::uint32_t kContinue = 7;
constexpr std::uint32_t kContinuec = 8;
constexpr std::uint32_t kDefault = 10;
constexpr std::uint32_t kDiscard = 13;
constexpr std::uint32_t kElse = 18;
constexpr std::uint32_t kEndIf = 21;
constexpr std::uint32_t kEndLoop = 22;
constexpr std::uint32_t kEndSwitch = 23;
constexpr std::uint32_t kIf = 31;
constexpr std::uint32_t kLabel = 44;
constexpr std::uint32_t kLoop = 48;
constexpr std::uint32_t kLt = 49;
constexpr std::uint32_t kMad = 50;
constexpr std::uint32_t kCustomData = 53;
constexpr std::uint32_t kMov = 54;
constexpr std::uint32_t kMul = 56;
constexpr std::uint32_t kRet = 62;
constexpr std::uint32_t kRetc = 63;
constexpr std::uint32_t kSample = 69;
constexpr std::uint32_t kSampleB = 74;
constexpr std::uint32_t kSwitch = 76;
constexpr std::uint32_t kDclResource = 88;
constexpr std::uint32_t kDclConstantBuffer = 89;
constexpr std::uint32_t kDclSampler = 90;
constexpr std::uint32_t kDclIndexRange = 91;
constexpr std::uint32_t kDclInputPs = 98;
constexpr std::uint32_t kDclInputPsSgv = 99;
constexpr std::uint32_t kDclInputPsSiv = 100;
constexpr std::uint32_t kDclOutput = 101;
constexpr std::uint32_t kDclOutputSgv = 102;
constexpr std::uint32_t kDclOutputSiv = 103;
constexpr std::uint32_t kDclTemps = 104;
constexpr std::uint32_t kDclIndexableTemp = 105;
constexpr std::uint32_t kDclGlobalFlags = 106;
constexpr std::uint32_t kDclHullShaderDeclarations = 113;
constexpr std::uint32_t kDclHullShaderJoinPhase = 116;
constexpr std::uint32_t kDclStream = 143;
constexpr std::uint32_t kDclUavTyped = 156;
constexpr std::uint32_t kDclUavRaw = 157;
constexpr std::uint32_t kDclUavStructured = 158;
constexpr std::uint32_t kDclResourceStructured = 162;
constexpr std::uint32_t kDclGeometryShaderInstanceCount = 206;
constexpr std::uint32_t kAtomicUmin = 177;
constexpr std::uint32_t kInterfaceCall = 120;
constexpr std::uint32_t kFtou = 28;
constexpr std::uint32_t kPositionSystemValue = 1;
constexpr std::uint32_t kCoverageSystemValue = 66;
constexpr std::uint32_t kOperandTypeTemporary = 0;
constexpr std::uint32_t kOperandTypeInput = 1;
constexpr std::uint32_t kOperandTypeOutput = 2;
constexpr std::uint32_t kOperandTypeImmediate32 = 4;
constexpr std::uint32_t kOperandTypeSampler = 6;
constexpr std::uint32_t kOperandTypeResource = 7;
constexpr std::uint32_t kOperandTypeConstantBuffer = 8;
constexpr std::uint32_t kOperandTypeOutputDepth = 12;
constexpr std::uint32_t kOperandTypeOutputCoverageMask = 15;
constexpr std::uint32_t kOperandTypeUav = 30;
constexpr std::uint32_t kOperandTypeOutputDepthGreaterEqual = 38;
constexpr std::uint32_t kOperandTypeOutputDepthLessEqual = 39;
constexpr std::uint32_t kForceEarlyDepthStencil = 1u << 13;
constexpr std::uint32_t kOpcodeControlMask = 0x00fff800u;
constexpr std::uint32_t kNonZeroTestControl = 1u << 18;
constexpr std::uint32_t kMaximumTemporaryRegisterCount = 4096;
constexpr std::size_t kMaximumInputSize = 16u * 1024u * 1024u;
constexpr std::size_t kMaximumChunkCount = 64;
constexpr char kUnsupportedSliceReason[] =
    "original shader alpha prefix cannot be sliced safely";

struct ReviewedContiguousFallback {
    std::size_t bytecode_size;
    std::array<std::uint8_t, 16> dxbc_checksum;
};

// These exact game shaders passed the legacy contiguous-prefix transform,
// D3D11 WARP creation, and native-versus-capture execution equivalence. Keep
// every other slice-unsupported shader fail-closed.
constexpr std::array<ReviewedContiguousFallback, 9>
    kReviewedContiguousFallbacks = {{
        {3456, {0xF7, 0x6B, 0x15, 0xFE, 0xB2, 0x69, 0xA1, 0xEC,
                0xC8, 0x03, 0x8B, 0x39, 0x36, 0xF9, 0xBC, 0xDF}},
        {6408, {0x68, 0xDA, 0x85, 0x0F, 0x28, 0xFB, 0xBE, 0x99,
                0x18, 0x05, 0x07, 0x41, 0x5A, 0x37, 0xA5, 0xFF}},
        {2484, {0x51, 0x0E, 0x6D, 0xC1, 0xDB, 0x4A, 0x3E, 0xAE,
                0x5B, 0x10, 0xF8, 0x7B, 0x03, 0xFB, 0x66, 0xEF}},
        {3460, {0xDE, 0xDF, 0xB5, 0xA7, 0xF0, 0x7D, 0xCE, 0x01,
                0x80, 0x16, 0xBF, 0x3B, 0x14, 0x2E, 0xA1, 0x82}},
        {1564, {0xEB, 0x53, 0x8F, 0x50, 0xBE, 0x7A, 0x7A, 0x68,
                0xCA, 0xDB, 0x42, 0x60, 0xE7, 0xAB, 0x97, 0x41}},
        {6100, {0x10, 0xE7, 0xEA, 0xF8, 0x64, 0x64, 0x4C, 0xFE,
                0x12, 0x0B, 0x27, 0x37, 0xB3, 0xD1, 0x11, 0xD3}},
        {1460, {0x85, 0xBD, 0xFF, 0xC0, 0xC2, 0x75, 0x96, 0xEC,
                0x29, 0xDB, 0xDE, 0x32, 0xD6, 0xE3, 0x6E, 0xB8}},
        {4072, {0xC0, 0x3B, 0xE0, 0x81, 0xA4, 0xAA, 0x96, 0x49,
                0x6F, 0xFA, 0x88, 0xC8, 0x85, 0xCA, 0xE5, 0xF4}},
        {4892, {0x22, 0x69, 0x4D, 0xFB, 0x05, 0x3F, 0x48, 0x0E,
                0xB4, 0x03, 0x7E, 0x49, 0xF3, 0x9A, 0x85, 0x0D}},
    }};

bool IsReviewedContiguousFallback(
    const std::vector<std::uint8_t> &bytecode) noexcept
{
    if (bytecode.size() < 20)
        return false;
    return std::any_of(
        kReviewedContiguousFallbacks.begin(),
        kReviewedContiguousFallbacks.end(),
        [&bytecode](const ReviewedContiguousFallback &identity) {
            return bytecode.size() == identity.bytecode_size &&
                std::equal(
                    identity.dxbc_checksum.begin(),
                    identity.dxbc_checksum.end(),
                    bytecode.begin() + 4);
        });
}

class TransformFailure final : public std::runtime_error
{
public:
    TransformFailure(InstrumentationStatus status, const char *reason)
        : std::runtime_error(reason), status_(status)
    {
    }

    [[nodiscard]] InstrumentationStatus Status() const noexcept
    {
        return status_;
    }

private:
    InstrumentationStatus status_;
};

[[noreturn]] void Reject(InstrumentationStatus status, const char *reason)
{
    throw TransformFailure(status, reason);
}

std::uint32_t ReadU32(const std::uint8_t *bytes)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void WriteU32(std::uint8_t *bytes, std::uint32_t value)
{
    std::memcpy(bytes, &value, sizeof(value));
}

void AppendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    const auto oldSize = bytes.size();
    bytes.resize(oldSize + sizeof(value));
    WriteU32(bytes.data() + oldSize, value);
}

std::uint32_t RotateLeft(std::uint32_t value, unsigned int count)
{
    return (value << count) | (value >> (32 - count));
}

void Md5Transform(std::array<std::uint32_t, 4> &state,
                  const std::uint8_t *block)
{
    static constexpr std::array<std::uint32_t, 64> constants = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static constexpr std::array<unsigned int, 64> rotations = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };

    std::array<std::uint32_t, 16> words = {};
    for (std::size_t index = 0; index < words.size(); ++index)
        words[index] = ReadU32(block + index * 4);

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    for (std::size_t index = 0; index < 64; ++index)
    {
        std::uint32_t function = 0;
        std::size_t word = 0;
        if (index < 16)
        {
            function = (b & c) | (~b & d);
            word = index;
        }
        else if (index < 32)
        {
            function = (d & b) | (~d & c);
            word = (5 * index + 1) & 15;
        }
        else if (index < 48)
        {
            function = b ^ c ^ d;
            word = (3 * index + 5) & 15;
        }
        else
        {
            function = c ^ (b | ~d);
            word = (7 * index) & 15;
        }

        const auto nextD = c;
        c = b;
        b += RotateLeft(a + function + constants[index] + words[word],
                        rotations[index]);
        a = d;
        d = nextD;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::array<std::uint32_t, 4> ComputeDxbcChecksum(
    const std::uint8_t *data, std::size_t size)
{
    std::array<std::uint32_t, 4> state = {
        0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
    };
    const auto bitLength = static_cast<std::uint32_t>(size << 3);
    while (size >= 64)
    {
        Md5Transform(state, data);
        data += 64;
        size -= 64;
    }

    std::array<std::uint8_t, 64> block = {};
    if (size <= 55)
    {
        WriteU32(block.data(), bitLength);
        std::copy_n(data, size, block.begin() + 4);
        block[4 + size] = 0x80;
        WriteU32(block.data() + 60, (bitLength >> 2) | 1);
        Md5Transform(state, block.data());
    }
    else
    {
        std::copy_n(data, size, block.begin());
        block[size] = 0x80;
        Md5Transform(state, block.data());
        block.fill(0);
        WriteU32(block.data(), bitLength);
        WriteU32(block.data() + 60, (bitLength >> 2) | 1);
        Md5Transform(state, block.data());
    }
    return state;
}

void SignDxbc(std::vector<std::uint8_t> &shader)
{
    if (shader.size() <= 20)
        throw std::runtime_error("DXBC container is too small to sign");
    const auto checksum = ComputeDxbcChecksum(
        shader.data() + 20, shader.size() - 20);
    std::memcpy(shader.data() + 4, checksum.data(), 16);
}

struct Chunk
{
    std::uint32_t tag = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<Chunk> ParseChunks(
    const std::vector<std::uint8_t> &shader,
    InstrumentationStatus invalid_status)
{
    if (shader.size() < 32 || ReadU32(shader.data()) != kDxbc)
        Reject(invalid_status, "not a DXBC container");
    if (ReadU32(shader.data() + 24) !=
        static_cast<std::uint32_t>(shader.size()))
        Reject(invalid_status, "DXBC container size does not match its header");

    auto checksum_probe = shader;
    SignDxbc(checksum_probe);
    if (!std::equal(shader.begin() + 4, shader.begin() + 20,
                    checksum_probe.begin() + 4))
        Reject(invalid_status, "DXBC checksum is invalid");

    const auto chunkCount = ReadU32(shader.data() + 28);
    if (chunkCount == 0 || chunkCount > kMaximumChunkCount ||
        shader.size() < 32ull + 4ull * chunkCount)
        Reject(invalid_status, "DXBC chunk table is invalid");

    std::vector<Chunk> chunks;
    chunks.reserve(chunkCount);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(chunkCount);
    for (std::uint32_t index = 0; index < chunkCount; ++index)
    {
        const auto offset = ReadU32(shader.data() + 32 + 4ull * index);
        if ((offset & 3u) != 0 || offset < 32 + 4ull * chunkCount ||
            offset + 8ull > shader.size())
            Reject(invalid_status, "DXBC chunk offset is invalid");
        const auto size = ReadU32(shader.data() + offset + 4);
        if (offset + 8ull + size > shader.size())
            Reject(invalid_status, "DXBC chunk is truncated");
        const std::size_t range_begin = offset;
        const std::size_t range_end = offset + 8ull + size;
        for (const auto &[existing_begin, existing_end] : ranges)
        {
            if (range_begin < existing_end && existing_begin < range_end)
                Reject(invalid_status, "DXBC chunks overlap");
        }
        ranges.emplace_back(range_begin, range_end);
        Chunk chunk;
        chunk.tag = ReadU32(shader.data() + offset);
        chunk.payload.assign(shader.begin() + offset + 8,
                             shader.begin() + offset + 8 + size);
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

std::vector<std::uint8_t> BuildContainer(const std::vector<std::uint8_t> &base,
                                         const std::vector<Chunk> &chunks)
{
    const auto headerSize = 32 + 4 * chunks.size();
    std::size_t total_size = headerSize;
    for (const Chunk &chunk : chunks)
    {
        if (total_size > kMaximumInputSize - 8 ||
            chunk.payload.size() > kMaximumInputSize - total_size - 8)
            Reject(
                InstrumentationStatus::OutputTooLarge,
                "instrumented DXBC container is too large");
        total_size += 8 + chunk.payload.size();
    }
    std::vector<std::uint8_t> result(headerSize, 0);
    result.reserve(total_size);
    std::copy_n(base.begin(), std::min<std::size_t>(24, base.size()),
                result.begin());
    WriteU32(result.data(), kDxbc);
    WriteU32(result.data() + 20, 1);
    WriteU32(result.data() + 28,
             static_cast<std::uint32_t>(chunks.size()));

    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
        WriteU32(result.data() + 32 + 4 * index,
                 static_cast<std::uint32_t>(result.size()));
        AppendU32(result, chunks[index].tag);
        AppendU32(result,
                  static_cast<std::uint32_t>(chunks[index].payload.size()));
        result.insert(result.end(), chunks[index].payload.begin(),
                      chunks[index].payload.end());
    }
    WriteU32(result.data() + 24,
             static_cast<std::uint32_t>(result.size()));
    SignDxbc(result);
    return result;
}

std::vector<std::uint32_t> BytesToTokens(
    const std::vector<std::uint8_t> &payload,
    InstrumentationStatus invalid_status)
{
    if (payload.size() % 4 != 0)
        Reject(invalid_status, "tokenized program is unaligned");
    std::vector<std::uint32_t> tokens(payload.size() / 4);
    std::memcpy(tokens.data(), payload.data(), payload.size());
    if (tokens.size() < 2 ||
        tokens[1] != static_cast<std::uint32_t>(tokens.size()))
        Reject(invalid_status, "tokenized program length is invalid");
    return tokens;
}

std::vector<std::uint8_t> TokensToBytes(
    const std::vector<std::uint32_t> &tokens)
{
    std::vector<std::uint8_t> bytes(tokens.size() * 4);
    std::memcpy(bytes.data(), tokens.data(), bytes.size());
    return bytes;
}

std::string SignatureName(const std::vector<std::uint8_t> &signature,
                          std::uint32_t offset,
                          InstrumentationStatus invalid_status =
                              InstrumentationStatus::InvalidOriginalContainer)
{
    if (offset >= signature.size())
        Reject(
            invalid_status,
            "signature semantic offset is invalid");
    const auto begin = reinterpret_cast<const char *>(signature.data() + offset);
    const auto available = signature.size() - offset;
    const auto end = static_cast<const char *>(
        std::memchr(begin, '\0', available));
    if (!end)
        Reject(
            invalid_status,
            "signature semantic is not terminated");
    return { begin, end };
}

void EnsurePositionSignature(std::vector<std::uint8_t> &signature)
{
    constexpr std::size_t headerSize = 8;
    constexpr std::size_t elementSize = 24;
    if (signature.size() < headerSize)
        Reject(
            InstrumentationStatus::InvalidOriginalContainer,
            "input signature is truncated");
    const auto count = ReadU32(signature.data());
    if (count > (signature.size() - headerSize) / elementSize)
        Reject(
            InstrumentationStatus::InvalidOriginalContainer,
            "input signature elements are truncated");

    std::uint8_t *position_element = nullptr;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        auto *element = signature.data() + headerSize + elementSize * index;
        const auto name = SignatureName(signature, ReadU32(element));
        const auto systemValue = ReadU32(element + 8);
        const auto registerIndex = ReadU32(element + 16);
        const bool is_position = systemValue == kPositionSystemValue ||
            _stricmp(name.c_str(), "SV_Position") == 0;
        if (is_position)
        {
            if (position_element != nullptr || registerIndex != 0 ||
                ReadU32(element + 12) != 3)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "SV_Position signature is duplicated, non-float, or not assigned to v0");
            position_element = element;
        }
        else if (registerIndex == 0)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "v0 is occupied and cannot receive SV_Position");
    }
    if (position_element != nullptr)
    {
        WriteU32(
            position_element + 20,
            ReadU32(position_element + 20) | 0x0000070f);
        return;
    }

    const auto oldSize = signature.size();
    std::vector<std::uint8_t> expanded(oldSize + elementSize + 12, 0xab);
    WriteU32(expanded.data(), count + 1);
    WriteU32(expanded.data() + 4, ReadU32(signature.data() + 4));

    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto *source = signature.data() + headerSize + elementSize * index;
        auto *destination = expanded.data() + headerSize + elementSize * index;
        std::copy_n(source, elementSize, destination);
        WriteU32(destination, ReadU32(destination) + elementSize);
    }

    auto *position = expanded.data() + headerSize + elementSize * count;
    const auto semanticOffset = static_cast<std::uint32_t>(oldSize + elementSize);
    WriteU32(position, semanticOffset);
    WriteU32(position + 4, 0);
    WriteU32(position + 8, 1); // D3D_NAME_POSITION
    WriteU32(position + 12, 3); // D3D_REGISTER_COMPONENT_FLOAT32
    WriteU32(position + 16, 0);
    WriteU32(position + 20, 0x0000070f);

    std::copy(signature.begin() + headerSize + elementSize * count,
              signature.end(),
              expanded.begin() + headerSize + elementSize * (count + 1));
    static constexpr std::array<char, 12> semantic = {
        'S', 'V', '_', 'P', 'o', 's', 'i', 't', 'i', 'o', 'n', '\0',
    };
    std::copy(semantic.begin(), semantic.end(),
              expanded.begin() + semanticOffset);
    signature = std::move(expanded);
}

void RejectUnsafeOutputSignature(const std::vector<std::uint8_t> &signature)
{
    constexpr std::size_t headerSize = 8;
    constexpr std::size_t elementSize = 24;
    if (signature.size() < headerSize)
        Reject(
            InstrumentationStatus::InvalidOriginalContainer,
            "output signature is truncated");
    const std::uint32_t count = ReadU32(signature.data());
    if (count > (signature.size() - headerSize) / elementSize)
        Reject(
            InstrumentationStatus::InvalidOriginalContainer,
            "output signature elements are truncated");
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto *element =
            signature.data() + headerSize + elementSize * index;
        const std::string name = SignatureName(
            signature, ReadU32(element));
        const std::uint32_t system_value = ReadU32(element + 8);
        if (_strnicmp(name.c_str(), "SV_Depth", 8) == 0)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader output signature contains SV_Depth");
        if (system_value == kCoverageSystemValue ||
            _stricmp(name.c_str(), "SV_Coverage") == 0)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader output signature contains SV_Coverage");
    }
}

void ValidateCaptureOutputSignature(
    const std::vector<std::uint8_t> &signature,
    bool require_exact_signature,
    InstrumentationStatus invalid_status)
{
    constexpr std::size_t headerSize = 8;
    constexpr std::size_t elementSize = 24;
    if (signature.size() < headerSize)
        Reject(invalid_status, "capture output signature is truncated");

    const std::uint32_t count = ReadU32(signature.data());
    if (count > (signature.size() - headerSize) / elementSize)
        Reject(
            invalid_status,
            "capture output signature elements are truncated");
    if (require_exact_signature && count != 2)
        Reject(
            invalid_status,
            "capture donor must expose exactly SV_Target0.xy and SV_Target1.xy");

    std::array<bool, 2> found = {false, false};
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto *element =
            signature.data() + headerSize + elementSize * index;
        const std::string name = SignatureName(
            signature, ReadU32(element), invalid_status);
        const std::uint32_t semantic_index = ReadU32(element + 4);
        const std::uint32_t component_type = ReadU32(element + 12);
        const std::uint32_t register_index = ReadU32(element + 16);
        const std::uint32_t component_mask = ReadU32(element + 20) & 0xffu;
        const bool is_capture_target =
            _stricmp(name.c_str(), "SV_Target") == 0 &&
            semantic_index < found.size();

        if (!is_capture_target)
        {
            if (require_exact_signature)
                Reject(
                    invalid_status,
                    "capture donor output signature contains a non-capture target");
            continue;
        }
        if (found[semantic_index] || register_index != semantic_index ||
            component_type != 3 || (component_mask & 0x3u) != 0x3u)
            Reject(
                invalid_status,
                "capture output targets must be unique float2 values at o0.xy and o1.xy");
        found[semantic_index] = true;
    }
    if (!found[0] || !found[1])
        Reject(
            invalid_status,
            "capture output signature lacks SV_Target0.xy or SV_Target1.xy");
}

void EnsureCaptureOutputSignature(
    std::vector<std::uint8_t> &signature,
    const std::vector<std::uint8_t> &donor_signature)
{
    constexpr std::size_t headerSize = 8;
    constexpr std::size_t elementSize = 24;
    constexpr std::array<char, 12> targetSemantic = {
        'S', 'V', '_', 'T', 'a', 'r', 'g', 'e', 't', '\0', '\0', '\0',
    };
    if (signature.size() < headerSize || donor_signature.size() < headerSize)
        Reject(
            InstrumentationStatus::InvalidOriginalContainer,
            "capture output signature is truncated");

    std::array<const std::uint8_t *, 2> donor_elements{};
    const std::uint32_t donor_count = ReadU32(donor_signature.data());
    if (donor_count >
        (donor_signature.size() - headerSize) / elementSize)
        Reject(
            InstrumentationStatus::InvalidCaptureDonor,
            "capture donor output signature elements are truncated");
    for (std::uint32_t index = 0; index < donor_count; ++index)
    {
        const auto *element =
            donor_signature.data() + headerSize + elementSize * index;
        const std::string name = SignatureName(
            donor_signature, ReadU32(element),
            InstrumentationStatus::InvalidCaptureDonor);
        const std::uint32_t semantic_index = ReadU32(element + 4);
        if (_stricmp(name.c_str(), "SV_Target") == 0 &&
            semantic_index < donor_elements.size())
            donor_elements[semantic_index] = element;
    }
    if (!donor_elements[0] || !donor_elements[1])
        Reject(
            InstrumentationStatus::InvalidCaptureDonor,
            "capture donor output signature lacks both capture targets");

    for (std::uint32_t target = 0; target < donor_elements.size(); ++target)
    {
        const std::uint32_t count = ReadU32(signature.data());
        if (count > (signature.size() - headerSize) / elementSize)
            Reject(
                InstrumentationStatus::InvalidOriginalContainer,
                "output signature elements are truncated");

        std::uint8_t *matching_element = nullptr;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            auto *element =
                signature.data() + headerSize + elementSize * index;
            const std::string name = SignatureName(
                signature, ReadU32(element));
            const std::uint32_t semantic_index = ReadU32(element + 4);
            const std::uint32_t register_index = ReadU32(element + 16);
            const bool is_target =
                _stricmp(name.c_str(), "SV_Target") == 0 &&
                semantic_index == target;
            if (is_target)
            {
                if (matching_element != nullptr || register_index != target ||
                    ReadU32(element + 12) != 3)
                    Reject(
                        InstrumentationStatus::UnsupportedOriginalShader,
                        "fallback capture target is duplicated, non-float, or assigned to the wrong register");
                matching_element = element;
            }
            else if (register_index == target)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "fallback output register conflicts with a required capture target");
        }

        if (matching_element != nullptr)
        {
            WriteU32(
                matching_element + 20,
                ReadU32(matching_element + 20) |
                    (ReadU32(donor_elements[target] + 20) & 0x0000ffffu));
            continue;
        }

        const std::size_t old_size = signature.size();
        std::vector<std::uint8_t> expanded(
            old_size + elementSize + targetSemantic.size(), 0);
        WriteU32(expanded.data(), count + 1);
        WriteU32(expanded.data() + 4, ReadU32(signature.data() + 4));
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const auto *source =
                signature.data() + headerSize + elementSize * index;
            auto *destination =
                expanded.data() + headerSize + elementSize * index;
            std::copy_n(source, elementSize, destination);
            WriteU32(destination, ReadU32(destination) + elementSize);
        }

        auto *capture_element =
            expanded.data() + headerSize + elementSize * count;
        std::copy_n(donor_elements[target], elementSize, capture_element);
        const auto semantic_offset =
            static_cast<std::uint32_t>(old_size + elementSize);
        WriteU32(capture_element, semantic_offset);
        std::copy(
            signature.begin() + headerSize + elementSize * count,
            signature.end(),
            expanded.begin() + headerSize + elementSize * (count + 1));
        std::copy(
            targetSemantic.begin(), targetSemantic.end(),
            expanded.begin() + semantic_offset);
        signature = std::move(expanded);
    }
}

std::uint32_t Opcode(std::uint32_t token)
{
    return token & 0x7ff;
}

bool IsUnsupportedOriginalControlFlow(std::uint32_t opcode) noexcept
{
    switch (opcode)
    {
    case kBreak:
    case kBreakc:
    case kCall:
    case kCallc:
    case kCase:
    case kContinue:
    case kContinuec:
    case kDefault:
    case kElse:
    case kEndIf:
    case kEndLoop:
    case kEndSwitch:
    case kIf:
    case kLabel:
    case kLoop:
    case kRetc:
    case kSwitch:
    case kInterfaceCall:
        return true;
    default:
        return false;
    }
}

bool IsKnownDeclarationOpcode(std::uint32_t opcode) noexcept
{
    return (opcode >= kDclResource && opcode <= kDclGlobalFlags) ||
        (opcode >= kDclHullShaderDeclarations &&
         opcode <= kDclHullShaderJoinPhase) ||
        (opcode >= kDclStream && opcode <= kDclResourceStructured) ||
        opcode == kDclGeometryShaderInstanceCount;
}

bool IsSupportedOriginalDeclarationOpcode(std::uint32_t opcode) noexcept
{
    switch (opcode)
    {
    case kDclResource:
    case kDclConstantBuffer:
    case kDclSampler:
    case kDclIndexRange:
    case kDclInputPs:
    case kDclInputPsSgv:
    case kDclInputPsSiv:
    case kDclOutput:
    case kDclOutputSgv:
    case kDclOutputSiv:
    case kDclTemps:
    case kDclIndexableTemp:
    case kDclGlobalFlags:
        return true;
    default:
        return false;
    }
}

std::size_t InstructionLength(
    const std::vector<std::uint32_t> &tokens,
    std::size_t index,
    InstrumentationStatus invalid_status)
{
    if (index >= tokens.size())
        Reject(invalid_status, "instruction starts outside the token stream");
    const std::uint32_t opcode = Opcode(tokens[index]);
    std::size_t length = (tokens[index] >> 24) & 0x7f;
    if (opcode == kCustomData)
    {
        if (index + 1 >= tokens.size())
            Reject(invalid_status, "custom-data instruction is truncated");
        length = tokens[index + 1];
    }
    if (length == 0 || length > tokens.size() - index)
        Reject(invalid_status, "instruction length is invalid");
    return length;
}

std::uint32_t OperandType(std::uint32_t operand_token) noexcept
{
    return (operand_token >> 12) & 0xff;
}

struct SliceOperand
{
    std::uint32_t type = 0;
    std::uint32_t component_count = 0;
    std::uint32_t selection_mode = 0;
    std::uint32_t component_mask = 0;
    std::array<std::uint32_t, 4> swizzle{};
    std::array<std::uint32_t, 2> indices{};
    std::uint32_t index_count = 0;
    bool extended = false;
};

enum class SliceInstructionKind : std::uint8_t
{
    ComponentWise,
    Sample,
    Discard,
};

struct SliceInstruction
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint32_t opcode = 0;
    SliceInstructionKind kind = SliceInstructionKind::ComponentWise;
    std::array<SliceOperand, 5> operands{};
    std::size_t operand_count = 0;
    std::uint32_t retained_mask = 0;
    bool keep = false;
};

[[noreturn]] void RejectUnsupportedSlice()
{
    Reject(
        InstrumentationStatus::UnsupportedOriginalShader,
        kUnsupportedSliceReason);
}

SliceOperand ParseSliceOperand(
    const std::vector<std::uint32_t> &tokens,
    std::size_t &cursor,
    std::size_t instruction_end,
    bool destination)
{
    if (cursor >= instruction_end)
        RejectUnsupportedSlice();

    SliceOperand result;
    const std::uint32_t token = tokens[cursor++];
    result.type = OperandType(token);
    result.component_count = token & 0x3u;
    result.selection_mode = (token >> 2) & 0x3u;
    result.component_mask = (token >> 4) & 0xfu;
    for (std::size_t component = 0; component < result.swizzle.size();
         ++component)
        result.swizzle[component] =
            (token >> (4 + 2 * component)) & 0x3u;

    if ((result.component_count == 0 || result.component_count == 1) &&
        (token & 0x00000ffcu) != 0)
        RejectUnsupportedSlice();
    if (result.component_count == 2)
    {
        if ((result.selection_mode == 0 &&
             (token & 0x00000f00u) != 0) ||
            (result.selection_mode == 2 &&
             (token & 0x00000fc0u) != 0) ||
            result.selection_mode == 3)
            RejectUnsupportedSlice();
    }
    else if (result.component_count > 2)
        RejectUnsupportedSlice();

    if ((token & 0x80000000u) != 0)
    {
        if (destination || cursor >= instruction_end)
            RejectUnsupportedSlice();
        const std::uint32_t extension = tokens[cursor++];
        const std::uint32_t extension_type = extension & 0x3fu;
        const std::uint32_t modifier = (extension >> 6) & 0xffu;
        const std::uint32_t minimum_precision = (extension >> 14) & 0x7u;
        if ((extension & 0x80000000u) != 0 || extension_type != 1 ||
            modifier > 3 || minimum_precision != 0 ||
            (extension & 0x7ffe0000u) != 0)
            RejectUnsupportedSlice();
        result.extended = true;
    }

    result.index_count = (token >> 20) & 0x3u;
    if (result.index_count > result.indices.size())
        RejectUnsupportedSlice();
    if ((result.index_count == 0 && (token & 0x7fc00000u) != 0) ||
        (result.index_count == 1 && (token & 0x7e000000u) != 0) ||
        (result.index_count == 2 && (token & 0x70000000u) != 0))
        RejectUnsupportedSlice();
    for (std::uint32_t index = 0; index < result.index_count; ++index)
    {
        const std::uint32_t representation =
            (token >> (22 + 3 * index)) & 0x7u;
        if (representation != 0 || cursor >= instruction_end)
            RejectUnsupportedSlice();
        result.indices[index] = tokens[cursor++];
    }

    if (result.type == kOperandTypeImmediate32)
    {
        if (result.index_count != 0)
            RejectUnsupportedSlice();
        std::size_t literal_count = 0;
        if (result.component_count == 1)
            literal_count = 1;
        else if (result.component_count == 2)
            literal_count = 4;
        else
            RejectUnsupportedSlice();
        if (literal_count > instruction_end - cursor)
            RejectUnsupportedSlice();
        cursor += literal_count;
    }
    return result;
}

bool IsSliceValueSource(const SliceOperand &operand) noexcept
{
    if (operand.type == kOperandTypeImmediate32)
        return operand.index_count == 0 &&
            (operand.component_count == 1 || operand.component_count == 2);
    if (operand.type == kOperandTypeTemporary ||
        operand.type == kOperandTypeInput)
        return operand.index_count == 1 && operand.component_count == 2 &&
            (operand.selection_mode == 1 || operand.selection_mode == 2);
    if (operand.type == kOperandTypeConstantBuffer)
        return operand.index_count == 2 && operand.component_count == 2 &&
            (operand.selection_mode == 1 || operand.selection_mode == 2);
    return false;
}

bool IsSliceDestination(const SliceOperand &operand) noexcept
{
    return !operand.extended &&
        (operand.type == kOperandTypeTemporary ||
         operand.type == kOperandTypeOutput) &&
        operand.index_count == 1 && operand.component_count == 2 &&
        operand.selection_mode == 0 && operand.component_mask != 0;
}

bool IsSliceResource(const SliceOperand &operand) noexcept
{
    return !operand.extended && operand.type == kOperandTypeResource &&
        operand.index_count == 1 && operand.component_count == 2 &&
        operand.selection_mode == 1;
}

bool IsSliceSampler(const SliceOperand &operand) noexcept
{
    return !operand.extended && operand.type == kOperandTypeSampler &&
        operand.index_count == 1 && operand.component_count == 0;
}

std::uint32_t SliceSourceComponents(
    const SliceOperand &operand,
    std::uint32_t needed_components) noexcept
{
    if (operand.component_count == 1)
        return 1;
    if (operand.selection_mode == 2)
        return 1u << operand.swizzle[0];
    std::uint32_t result = 0;
    for (std::size_t component = 0; component < operand.swizzle.size();
         ++component)
    {
        if ((needed_components & (1u << component)) != 0)
            result |= 1u << operand.swizzle[component];
    }
    return result;
}

void AddSliceDependency(
    const SliceOperand &operand,
    std::uint32_t components,
    std::vector<std::uint8_t> &live)
{
    if (operand.type != kOperandTypeTemporary)
        return;
    if (operand.index_count != 1 || operand.indices[0] >= live.size())
        RejectUnsupportedSlice();
    live[operand.indices[0]] |= static_cast<std::uint8_t>(components);
}

SliceInstruction ParseSliceInstruction(
    const std::vector<std::uint32_t> &tokens,
    std::size_t begin,
    std::size_t end)
{
    if (begin >= end || (tokens[begin] & 0x80000000u) != 0)
        RejectUnsupportedSlice();

    SliceInstruction result;
    result.begin = begin;
    result.end = end;
    result.opcode = Opcode(tokens[begin]);
    const std::uint32_t opcode_controls =
        tokens[begin] & kOpcodeControlMask;
    switch (result.opcode)
    {
    case kAdd:
    case kLt:
    case kMov:
    case kMul:
        result.kind = SliceInstructionKind::ComponentWise;
        result.operand_count = result.opcode == kMov ? 2 : 3;
        break;
    case kMad:
        result.kind = SliceInstructionKind::ComponentWise;
        result.operand_count = 4;
        break;
    case kSample:
        result.kind = SliceInstructionKind::Sample;
        result.operand_count = 4;
        break;
    case kSampleB:
        result.kind = SliceInstructionKind::Sample;
        result.operand_count = 5;
        break;
    case kDiscard:
        result.kind = SliceInstructionKind::Discard;
        result.operand_count = 1;
        break;
    default:
        RejectUnsupportedSlice();
    }
    if ((result.kind == SliceInstructionKind::Discard &&
         opcode_controls != kNonZeroTestControl) ||
        (result.kind != SliceInstructionKind::Discard &&
         opcode_controls != 0))
        RejectUnsupportedSlice();

    std::size_t cursor = begin + 1;
    for (std::size_t operand = 0; operand < result.operand_count; ++operand)
        result.operands[operand] = ParseSliceOperand(
            tokens, cursor, end,
            operand == 0 && result.kind != SliceInstructionKind::Discard);
    if (cursor != end)
        RejectUnsupportedSlice();

    if (result.kind == SliceInstructionKind::Discard)
    {
        if (!IsSliceValueSource(result.operands[0]))
            RejectUnsupportedSlice();
    }
    else
    {
        if (!IsSliceDestination(result.operands[0]))
            RejectUnsupportedSlice();
        if (result.kind == SliceInstructionKind::ComponentWise)
        {
            for (std::size_t operand = 1;
                 operand < result.operand_count; ++operand)
            {
                if (!IsSliceValueSource(result.operands[operand]))
                    RejectUnsupportedSlice();
            }
        }
        else
        {
            if (!IsSliceValueSource(result.operands[1]) ||
                !IsSliceResource(result.operands[2]) ||
                !IsSliceSampler(result.operands[3]) ||
                (result.operand_count == 5 &&
                 !IsSliceValueSource(result.operands[4])))
                RejectUnsupportedSlice();
        }
    }
    return result;
}

std::vector<std::uint32_t> SliceAlphaPrefix(
    const std::vector<std::uint32_t> &tokens,
    std::size_t begin,
    std::size_t end,
    std::uint32_t temporary_count)
{
    if (begin >= end || temporary_count == 0 ||
        temporary_count > kMaximumTemporaryRegisterCount)
        RejectUnsupportedSlice();

    std::vector<SliceInstruction> instructions;
    for (std::size_t cursor = begin; cursor < end;)
    {
        const std::size_t length = InstructionLength(
            tokens, cursor, InstrumentationStatus::UnsupportedOriginalShader);
        if (length > end - cursor)
            RejectUnsupportedSlice();
        instructions.push_back(ParseSliceInstruction(
            tokens, cursor, cursor + length));
        cursor += length;
    }
    if (instructions.empty() || instructions.back().opcode != kDiscard)
        RejectUnsupportedSlice();

    std::vector<std::uint8_t> live(temporary_count, 0);
    for (auto iterator = instructions.rbegin();
         iterator != instructions.rend(); ++iterator)
    {
        SliceInstruction &instruction = *iterator;
        if (instruction.kind == SliceInstructionKind::Discard)
        {
            instruction.keep = true;
            const SliceOperand &predicate = instruction.operands[0];
            AddSliceDependency(
                predicate, SliceSourceComponents(predicate, 0xfu), live);
            continue;
        }

        const SliceOperand &destination = instruction.operands[0];
        if (destination.type != kOperandTypeTemporary)
            continue;
        if (destination.indices[0] >= live.size())
            RejectUnsupportedSlice();
        const std::uint32_t needed =
            live[destination.indices[0]] & destination.component_mask;
        if (needed == 0)
            continue;

        instruction.keep = true;
        instruction.retained_mask = needed;
        live[destination.indices[0]] &=
            static_cast<std::uint8_t>(~destination.component_mask);
        if (instruction.kind == SliceInstructionKind::ComponentWise)
        {
            for (std::size_t operand = 1;
                 operand < instruction.operand_count; ++operand)
            {
                const SliceOperand &source = instruction.operands[operand];
                AddSliceDependency(
                    source, SliceSourceComponents(source, needed), live);
            }
        }
        else
        {
            const SliceOperand &coordinates = instruction.operands[1];
            AddSliceDependency(
                coordinates,
                SliceSourceComponents(coordinates, 0xfu), live);
            if (instruction.operand_count == 5)
            {
                const SliceOperand &bias = instruction.operands[4];
                AddSliceDependency(
                    bias, SliceSourceComponents(bias, 0xfu), live);
            }
        }
    }
    if (std::any_of(live.begin(), live.end(),
                    [](std::uint8_t components) { return components != 0; }))
        RejectUnsupportedSlice();

    std::vector<std::uint32_t> sliced;
    sliced.reserve(end - begin);
    for (const SliceInstruction &instruction : instructions)
    {
        if (instruction.keep)
        {
            const std::size_t destination = sliced.size() + 1;
            sliced.insert(
                sliced.end(), tokens.begin() + instruction.begin,
                tokens.begin() + instruction.end);
            if (instruction.kind != SliceInstructionKind::Discard)
            {
                sliced[destination] =
                    (sliced[destination] & ~0xf0u) |
                    (instruction.retained_mask << 4);
            }
        }
    }
    return sliced;
}

bool IsImmediateRegisterOperand(
    const std::vector<std::uint32_t> &tokens,
    std::size_t instruction,
    std::size_t length,
    std::uint32_t expected_type,
    std::uint32_t expected_register) noexcept
{
    if (length < 3 || instruction + 2 >= tokens.size())
        return false;
    const std::uint32_t operand = tokens[instruction + 1];
    const std::uint32_t index_dimension = (operand >> 20) & 0x3;
    const std::uint32_t index_representation = (operand >> 22) & 0x3;
    return OperandType(operand) == expected_type && index_dimension == 1 &&
        index_representation == 0 &&
        tokens[instruction + 2] == expected_register;
}

bool IsPositionDeclaration(
    const std::vector<std::uint32_t> &tokens,
    std::size_t instruction,
    std::size_t length) noexcept
{
    return length == 4 && instruction + 3 < tokens.size() &&
        Opcode(tokens[instruction]) == kDclInputPsSiv &&
        IsImmediateRegisterOperand(
            tokens, instruction, length, kOperandTypeInput, 0) &&
        (tokens[instruction + 3] & 0xffffu) == kPositionSystemValue;
}

struct DonorParts
{
    std::vector<std::uint32_t> position_declaration;
    std::vector<std::uint32_t> output_declarations;
    std::vector<std::uint32_t> execution;
    std::uint32_t temporary_count = 0;
};

DonorParts ValidateCaptureDonor(const std::vector<std::uint32_t> &helper)
{
    if (helper.size() < 2 || helper[0] != 0x00000050)
        Reject(
            InstrumentationStatus::InvalidCaptureDonor,
            "capture donor is not ps_5_0");

    DonorParts parts;
    bool execution_started = false;
    std::uint32_t global_flags = 0;
    std::uint32_t position_count = 0;
    std::uint32_t output_count = 0;
    std::uint32_t temps_count = 0;
    std::uint32_t discard_count = 0;
    std::uint32_t return_count = 0;

    for (std::size_t index = 2; index < helper.size();)
    {
        const std::uint32_t opcode = Opcode(helper[index]);
        const std::size_t length = InstructionLength(
            helper, index, InstrumentationStatus::InvalidCaptureDonor);
        if (opcode == kCustomData)
            Reject(
                InstrumentationStatus::InvalidCaptureDonor,
                "capture donor contains custom data");

        if (!execution_started)
        {
            if (opcode == kDclGlobalFlags)
            {
                if (length != 1 || global_flags != 0)
                    Reject(
                        InstrumentationStatus::InvalidCaptureDonor,
                        "capture donor global flags are invalid");
                global_flags = helper[index] & 0x00fff800u;
            }
            else if (opcode == kDclOutput)
            {
                const std::uint32_t operand =
                    length >= 2 ? helper[index + 1] : 0;
                if (length != 3 || output_count >= 2 ||
                    !IsImmediateRegisterOperand(
                        helper, index, length, kOperandTypeOutput, output_count) ||
                    (operand & 0x3u) != 2u ||
                    ((operand >> 2) & 0x3u) != 0u ||
                    ((operand >> 4) & 0xfu) != 0x3u)
                    Reject(
                        InstrumentationStatus::InvalidCaptureDonor,
                        "capture donor must declare float2 outputs o0 and o1 in order");
                parts.output_declarations.insert(
                    parts.output_declarations.end(),
                    helper.begin() + index,
                    helper.begin() + index + length);
                ++output_count;
            }
            else if (opcode == kDclInputPsSiv)
            {
                ++position_count;
                if (position_count != 1 ||
                    !IsPositionDeclaration(helper, index, length))
                    Reject(
                        InstrumentationStatus::InvalidCaptureDonor,
                        "capture donor must declare only SV_Position.xyz at v0");
                parts.position_declaration.assign(
                    helper.begin() + index, helper.begin() + index + length);
            }
            else if (opcode == kDclTemps)
            {
                ++temps_count;
                if (temps_count != 1 || length != 2 ||
                    helper[index + 1] == 0 || helper[index + 1] > 64)
                    Reject(
                        InstrumentationStatus::InvalidCaptureDonor,
                        "capture donor temporary declaration is invalid");
                parts.temporary_count = helper[index + 1];
            }
            else
            {
                if (opcode != kFtou)
                    Reject(
                        InstrumentationStatus::InvalidCaptureDonor,
                        "capture donor execution must begin with ftou");
                execution_started = true;
            }
        }

        if (execution_started)
        {
            if (opcode == kRetc)
                Reject(
                    InstrumentationStatus::InvalidCaptureDonor,
                    "capture donor contains a conditional return");
            if (opcode == kDclOutput || opcode == kDclOutputSgv ||
                opcode == kDclOutputSiv || opcode == 88 || opcode == 89 ||
                opcode == 90 || opcode == kDclUavTyped ||
                opcode == kDclUavRaw || opcode == kDclUavStructured ||
                opcode == 161 || opcode == 162)
                Reject(
                    InstrumentationStatus::InvalidCaptureDonor,
                    "capture donor contains a declaration after execution began");
            if (opcode >= 163 && opcode <= 189)
                Reject(
                    InstrumentationStatus::InvalidCaptureDonor,
                    "capture donor contains an unsupported memory effect");
            if (opcode == kDiscard)
                ++discard_count;
            else if (opcode == kRet)
            {
                ++return_count;
                if (index + length != helper.size())
                    Reject(
                        InstrumentationStatus::InvalidCaptureDonor,
                        "capture donor return is not final");
            }
            parts.execution.insert(
                parts.execution.end(),
                helper.begin() + index,
                helper.begin() + index + length);
        }
        index += length;
    }

    if (!execution_started ||
        (global_flags != 0 && global_flags != (1u << 11)) ||
        parts.position_declaration.empty() || parts.output_declarations.empty() ||
        parts.execution.empty() || parts.temporary_count == 0 ||
        position_count != 1 || output_count != 2 || temps_count != 1 ||
        discard_count != 1 || return_count != 1)
        Reject(
            InstrumentationStatus::InvalidCaptureDonor,
            "capture donor does not match the reviewed stochastic MIN-blend shape");
    return parts;
}

struct InstrumentedProgram {
    std::vector<std::uint32_t> tokens;
    bool used_contiguous_fallback = false;
};

InstrumentedProgram Instrument(
    const std::vector<std::uint32_t> &base,
    const DonorParts &helper,
    bool allow_contiguous_fallback)
{
    if (base.size() < 2 ||
        (base[0] != 0x00000040 && base[0] != 0x00000041))
        Reject(
            InstrumentationStatus::UnsupportedOriginalShader,
            "original shader is not ps_4_0 or ps_4_1");

    std::vector<std::uint32_t> result;
    result.reserve(
        base.size() + helper.position_declaration.size() +
        helper.output_declarations.size() + helper.execution.size());
    result.push_back(0x00000050); // ps_5_0
    result.push_back(0);          // fixed after instrumentation

    std::vector<std::uint32_t> sliced_declarations;
    sliced_declarations.reserve(result.capacity());
    sliced_declarations.push_back(0x00000050); // ps_5_0
    sliced_declarations.push_back(0);          // fixed after instrumentation

    bool position_declared = false;
    bool declarations_injected = false;
    bool execution_started = false;
    std::size_t declaration_prefix_size = 0;
    std::size_t capture_prefix_size = 0;
    std::uint32_t temporary_register_count = 0;
    std::uint32_t temporary_declaration_count = 0;
    std::uint32_t discard_count = 0;
    std::uint32_t return_count = 0;
    std::array<bool, 2> capture_output_declared = {false, false};
    for (std::size_t index = 2; index < base.size();)
    {
        const std::uint32_t opcode = Opcode(base[index]);
        const std::size_t length = InstructionLength(
            base, index, InstrumentationStatus::UnsupportedOriginalShader);
        if (opcode == kCustomData)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader contains custom data");
        if (IsUnsupportedOriginalControlFlow(opcode))
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader contains unsupported control flow");
        if (opcode == kDclUavTyped || opcode == kDclUavRaw ||
            opcode == kDclUavStructured)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader already declares a UAV");
        if (opcode == kDclGlobalFlags &&
            (base[index] & kForceEarlyDepthStencil) != 0)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader forces early depth-stencil");
        if (opcode == kDclOutput || opcode == kDclOutputSgv ||
            opcode == kDclOutputSiv)
        {
            if (length < 2)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader output declaration is invalid");
            const std::uint32_t output_type = OperandType(base[index + 1]);
            if (output_type == kOperandTypeOutputDepth ||
                output_type == kOperandTypeOutputDepthGreaterEqual ||
                output_type == kOperandTypeOutputDepthLessEqual)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader writes SV_Depth");
            if (output_type == kOperandTypeOutputCoverageMask)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader declares SV_Coverage");
            if (output_type != kOperandTypeOutput ||
                (opcode == kDclOutput && length < 3) ||
                (opcode != kDclOutput && length < 4))
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader output declaration shape is unsupported");
        }

        const bool is_declaration =
            IsSupportedOriginalDeclarationOpcode(opcode);
        if (IsKnownDeclarationOpcode(opcode) && !is_declaration)
            Reject(
                InstrumentationStatus::UnsupportedOriginalShader,
                "original shader contains an unsupported declaration");
        if (is_declaration)
        {
            if (execution_started)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader contains a declaration after execution began");
        }
        else
            execution_started = true;

        if (opcode == kDclInputPsSiv &&
            IsPositionDeclaration(base, index, length))
        {
            if (position_declared || declarations_injected)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader has duplicate or reordered SV_Position declarations");
            result.insert(
                result.end(),
                helper.position_declaration.begin(),
                helper.position_declaration.end());
            sliced_declarations.insert(
                sliced_declarations.end(),
                helper.position_declaration.begin(),
                helper.position_declaration.end());
            position_declared = true;
        }
        else if (opcode == kDclTemps)
        {
            ++temporary_declaration_count;
            if (temporary_declaration_count != 1 || length != 2)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader temporary declaration is invalid");
            if (!position_declared)
            {
                result.insert(
                    result.end(),
                    helper.position_declaration.begin(),
                    helper.position_declaration.end());
                sliced_declarations.insert(
                    sliced_declarations.end(),
                    helper.position_declaration.begin(),
                    helper.position_declaration.end());
                position_declared = true;
            }
            for (std::uint32_t output = 0;
                 output < capture_output_declared.size(); ++output)
            {
                if (!capture_output_declared[output])
                {
                    const auto begin =
                        helper.output_declarations.begin() + output * 3;
                    result.insert(result.end(), begin, begin + 3);
                    capture_output_declared[output] = true;
                }
            }
            sliced_declarations.insert(
                sliced_declarations.end(),
                helper.output_declarations.begin(),
                helper.output_declarations.end());
            result.insert(result.end(), base.begin() + index,
                          base.begin() + index + length);
            result.back() = (std::max)(
                result.back(), helper.temporary_count);
            sliced_declarations.insert(
                sliced_declarations.end(), base.begin() + index,
                base.begin() + index + length);
            sliced_declarations.back() = result.back();
            temporary_register_count = result.back();
            declarations_injected = true;
        }
        else if (opcode == kDclOutput || opcode == kDclOutputSgv ||
                 opcode == kDclOutputSiv)
        {
            // Retain native declarations only for the reviewed contiguous
            // fallback. The normal sliced derivative uses the donor's two
            // float2 capture targets.
            std::uint32_t capture_output = 0;
            bool is_capture_output = false;
            for (; capture_output < capture_output_declared.size();
                 ++capture_output)
            {
                if (IsImmediateRegisterOperand(
                        base, index, length, kOperandTypeOutput,
                        capture_output))
                {
                    is_capture_output = true;
                    break;
                }
            }
            if (is_capture_output)
            {
                if (opcode != kDclOutput || length != 3 ||
                    declarations_injected ||
                    capture_output_declared[capture_output])
                    Reject(
                        InstrumentationStatus::UnsupportedOriginalShader,
                        "fallback capture output declaration is unsupported, duplicated, or reordered");
                const std::uint32_t operand = base[index + 1];
                if ((operand & 0x3u) != 2u ||
                    ((operand >> 2) & 0x3u) != 0u)
                    Reject(
                        InstrumentationStatus::UnsupportedOriginalShader,
                        "fallback capture output declaration does not use a component mask");
                const std::size_t destination = result.size();
                result.insert(result.end(), base.begin() + index,
                              base.begin() + index + length);
                result[destination + 1] |= 0x3u << 4;
                capture_output_declared[capture_output] = true;
            }
            else
                result.insert(result.end(), base.begin() + index,
                              base.begin() + index + length);
        }
        else if (opcode == kRet)
        {
            ++return_count;
            if (return_count != 1 || index + length != base.size())
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader must have one final return");
        }
        else
        {
            result.insert(result.end(), base.begin() + index,
                          base.begin() + index + length);
            if (is_declaration)
            {
                sliced_declarations.insert(
                    sliced_declarations.end(), base.begin() + index,
                    base.begin() + index + length);
            }
        }
        if (is_declaration)
            declaration_prefix_size = result.size();
        if (opcode == kDiscard)
        {
            ++discard_count;
            capture_prefix_size = result.size();
        }
        index += length;
    }
    if (return_count != 1)
        Reject(
            InstrumentationStatus::UnsupportedOriginalShader,
            "original shader must have exactly one final return");
    if (!position_declared || !declarations_injected ||
        temporary_declaration_count != 1)
        Reject(
            InstrumentationStatus::UnsupportedOriginalShader,
            "original shader lacks the declarations required for instrumentation");
    if (discard_count == 0)
        capture_prefix_size = declaration_prefix_size;
    if (capture_prefix_size == 0)
        Reject(
            InstrumentationStatus::UnsupportedOriginalShader,
            "original shader has no safe capture prefix");

    std::vector<std::uint32_t> sliced_prefix;
    bool used_contiguous_fallback = false;
    if (discard_count != 0)
    {
        try
        {
            sliced_prefix = SliceAlphaPrefix(
                result, declaration_prefix_size, capture_prefix_size,
                temporary_register_count);
        }
        catch (const TransformFailure &failure)
        {
            if (!allow_contiguous_fallback ||
                failure.Status() !=
                    InstrumentationStatus::UnsupportedOriginalShader ||
                std::strcmp(failure.what(), kUnsupportedSliceReason) != 0)
                throw;
            used_contiguous_fallback = true;
        }
    }
    if (used_contiguous_fallback)
    {
        result.resize(capture_prefix_size);
    }
    else
    {
        result = std::move(sliced_declarations);
        result.insert(
            result.end(), sliced_prefix.begin(), sliced_prefix.end());
    }
    result.insert(
        result.end(), helper.execution.begin(), helper.execution.end());
    result[1] = static_cast<std::uint32_t>(result.size());
    return {std::move(result), used_contiguous_fallback};
}

}  // namespace

namespace spatch::graphics::sdao::dxbc {

namespace {

InstrumentationResult Failure(
    InstrumentationStatus status,
    const char *reason) noexcept
{
    InstrumentationResult result;
    result.status = status;
    if (reason != nullptr)
    {
        const std::size_t length = (std::min)(
            std::strlen(reason), result.rejection_reason.size() - 1);
        std::memcpy(result.rejection_reason.data(), reason, length);
        result.rejection_reason[length] = '\0';
    }
    return result;
}

}  // namespace

const char *StatusName(InstrumentationStatus status) noexcept
{
    switch (status)
    {
    case InstrumentationStatus::Success:
        return "success";
    case InstrumentationStatus::EmptyInput:
        return "empty-input";
    case InstrumentationStatus::InputTooLarge:
        return "input-too-large";
    case InstrumentationStatus::InvalidOriginalContainer:
        return "invalid-original-container";
    case InstrumentationStatus::InvalidCaptureDonor:
        return "invalid-capture-donor";
    case InstrumentationStatus::UnsupportedOriginalShader:
        return "unsupported-original-shader";
    case InstrumentationStatus::OutputTooLarge:
        return "output-too-large";
    case InstrumentationStatus::OutOfMemory:
        return "out-of-memory";
    case InstrumentationStatus::InternalFailure:
        return "internal-failure";
    }
    return "unknown";
}

InstrumentationResult InstrumentPixelShader(
    const void *original_bytecode,
    std::size_t original_size,
    const void *capture_donor_bytecode,
    std::size_t capture_donor_size) noexcept
{
    if (original_bytecode == nullptr || capture_donor_bytecode == nullptr ||
        original_size == 0 || capture_donor_size == 0)
        return Failure(
            InstrumentationStatus::EmptyInput,
            "DXBC instrumentation received an empty bytecode blob");
    if (original_size > kMaximumInputSize ||
        capture_donor_size > kMaximumInputSize)
        return Failure(
            InstrumentationStatus::InputTooLarge,
            "DXBC instrumentation input exceeds the bounded size limit");

    try
    {
        const auto *original_bytes =
            static_cast<const std::uint8_t *>(original_bytecode);
        const auto *donor_bytes =
            static_cast<const std::uint8_t *>(capture_donor_bytecode);
        std::vector<std::uint8_t> original(
            original_bytes, original_bytes + original_size);
        const std::vector<std::uint8_t> donor(
            donor_bytes, donor_bytes + capture_donor_size);

        auto original_chunks = ParseChunks(
            original, InstrumentationStatus::InvalidOriginalContainer);
        const auto donor_chunks = ParseChunks(
            donor, InstrumentationStatus::InvalidCaptureDonor);

        auto original_code = original_chunks.end();
        std::uint32_t original_code_count = 0;
        for (auto iterator = original_chunks.begin();
             iterator != original_chunks.end(); ++iterator)
        {
            if (iterator->tag == kShdr)
            {
                original_code = iterator;
                ++original_code_count;
            }
            else if (iterator->tag == kShex)
                Reject(
                    InstrumentationStatus::UnsupportedOriginalShader,
                    "original shader already contains a SHEX program");
        }
        if (original_code_count != 1 || original_code == original_chunks.end())
            Reject(
                InstrumentationStatus::InvalidOriginalContainer,
                "original shader must contain exactly one SHDR program");

        auto donor_code = donor_chunks.end();
        std::uint32_t donor_code_count = 0;
        for (auto iterator = donor_chunks.begin();
             iterator != donor_chunks.end(); ++iterator)
        {
            if (iterator->tag == kShex)
            {
                donor_code = iterator;
                ++donor_code_count;
            }
            else if (iterator->tag == kShdr)
                Reject(
                    InstrumentationStatus::InvalidCaptureDonor,
                    "capture donor contains an SHDR program");
        }
        if (donor_code_count != 1 || donor_code == donor_chunks.end())
            Reject(
                InstrumentationStatus::InvalidCaptureDonor,
                "capture donor must contain exactly one SHEX program");

        auto donor_output_signature = donor_chunks.end();
        std::uint32_t donor_output_signature_count = 0;
        for (auto iterator = donor_chunks.begin();
             iterator != donor_chunks.end(); ++iterator)
        {
            if (iterator->tag == kOsgn)
            {
                donor_output_signature = iterator;
                ++donor_output_signature_count;
            }
        }
        if (donor_output_signature_count != 1 ||
            donor_output_signature == donor_chunks.end())
            Reject(
                InstrumentationStatus::InvalidCaptureDonor,
                "capture donor must contain exactly one output signature");
        ValidateCaptureOutputSignature(
            donor_output_signature->payload, true,
            InstrumentationStatus::InvalidCaptureDonor);

        const DonorParts donor_parts = ValidateCaptureDonor(BytesToTokens(
            donor_code->payload,
            InstrumentationStatus::InvalidCaptureDonor));
        const auto instrumented_program = Instrument(
            BytesToTokens(
                original_code->payload,
                InstrumentationStatus::InvalidOriginalContainer),
            donor_parts,
            IsReviewedContiguousFallback(original));
        original_code->tag = kShex;
        original_code->payload = TokensToBytes(instrumented_program.tokens);

        auto input_signature = std::find_if(
            original_chunks.begin(), original_chunks.end(),
            [](const Chunk &chunk) { return chunk.tag == kIsgn; });
        auto output_signature = std::find_if(
            original_chunks.begin(), original_chunks.end(),
            [](const Chunk &chunk) { return chunk.tag == kOsgn; });
        if (input_signature == original_chunks.end() ||
            output_signature == original_chunks.end())
            Reject(
                InstrumentationStatus::InvalidOriginalContainer,
                "original shader input or output signature is missing");
        EnsurePositionSignature(input_signature->payload);
        RejectUnsafeOutputSignature(output_signature->payload);
        if (instrumented_program.used_contiguous_fallback)
        {
            EnsureCaptureOutputSignature(
                output_signature->payload,
                donor_output_signature->payload);
            ValidateCaptureOutputSignature(
                output_signature->payload, false,
                InstrumentationStatus::UnsupportedOriginalShader);
        }
        else
            output_signature->payload = donor_output_signature->payload;

        original_chunks.erase(
            std::remove_if(
                original_chunks.begin(),
                original_chunks.end(),
                [](const Chunk &chunk) {
                    return chunk.tag == kRdef || chunk.tag == kStat;
                }),
            original_chunks.end());

        InstrumentationResult result;
        result.status = InstrumentationStatus::Success;
        result.used_contiguous_fallback =
            instrumented_program.used_contiguous_fallback;
        result.bytecode = BuildContainer(original, original_chunks);
        return result;
    }
    catch (const TransformFailure &failure)
    {
        return Failure(failure.Status(), failure.what());
    }
    catch (const std::bad_alloc &)
    {
        return Failure(
            InstrumentationStatus::OutOfMemory,
            "DXBC instrumentation ran out of memory");
    }
    catch (const std::exception &failure)
    {
        return Failure(
            InstrumentationStatus::InternalFailure,
            failure.what());
    }
    catch (...)
    {
        return Failure(
            InstrumentationStatus::InternalFailure,
            "DXBC instrumentation failed unexpectedly");
    }
}

}  // namespace spatch::graphics::sdao::dxbc
