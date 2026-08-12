#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace spatch::graphics::sdao::dxbc {

enum class InstrumentationStatus : std::uint8_t {
    Success = 0,
    EmptyInput,
    InputTooLarge,
    InvalidOriginalContainer,
    InvalidCaptureDonor,
    UnsupportedOriginalShader,
    OutputTooLarge,
    OutOfMemory,
    InternalFailure,
};

struct InstrumentationResult {
    InstrumentationStatus status = InstrumentationStatus::InternalFailure;
    std::vector<std::uint8_t> bytecode;
    std::array<char, 192> rejection_reason{};
    bool used_contiguous_fallback = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == InstrumentationStatus::Success && !bytecode.empty();
    }

    [[nodiscard]] const char* Reason() const noexcept {
        return rejection_reason.data();
    }
};

// Builds a capture-only ps_5_0 derivative of a verified ps_4_0/ps_4_1 DXBC
// pixel shader. It preserves the original declaration stream plus the bounded
// data-flow slice required by the final alpha discard, then appends the trusted
// capture donor's two-target MIN-blend tail. Shading and native color outputs
// are omitted;
// a separate native draw retains the game's MRT/depth behavior and early-depth
// optimization. Nine exact reviewed identities may use the contiguous prefix
// fallback when their legal alpha prefix exceeds the bounded slicer grammar.
// The implementation rejects all other unreviewed
// opcodes, opcode controls, operand forms, control flow, calls, labels,
// conditional returns, coverage/depth output, reordered/unsupported
// declarations, or a non-final/multiple return. It then appends the execution
// and sole final return from a trusted capture donor. The donor is accepted only
// when it declares SV_Position, temporary registers, and exactly two float2
// color outputs at SV_Target0/1. It must contain one selection discard and no
// resource, UAV, sampler, constant-buffer, or memory side effects.
//
// No exception escapes this function. A rejected shader returns an empty
// bytecode vector and a bounded diagnostic suitable for an init-pipeline log.
[[nodiscard]] InstrumentationResult InstrumentPixelShader(
    const void* original_bytecode,
    std::size_t original_size,
    const void* capture_donor_bytecode,
    std::size_t capture_donor_size) noexcept;

[[nodiscard]] const char* StatusName(
    InstrumentationStatus status) noexcept;

}  // namespace spatch::graphics::sdao::dxbc
