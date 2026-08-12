#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spatch::runtime_patch {


enum class ByteState {
    Expected,
    Replacement,
    Unexpected,
};

enum class ApplyResult {
    Applied,
    AlreadyApplied,
    InvalidRequest,
    AllocationFailed,
    ReadFailed,
    UnexpectedBytes,
    WriteFailed,
};

[[nodiscard]] ByteState ClassifyBytes(std::span<const std::uint8_t> current,
                                      std::span<const std::uint8_t> expected,
                                      std::span<const std::uint8_t> replacement);
[[nodiscard]] const char* ApplyResultName(ApplyResult result);
[[nodiscard]] bool IsAddressInPatchRange(std::uintptr_t instruction_pointer,
                                         std::uintptr_t patch_address,
                                         std::size_t patch_size) noexcept;
// A thread that disappears between the Toolhelp snapshot and suspension may
// have created a successor that is absent from that fixed snapshot.  Such a
// pass is not stable even when it did not add a handle to the frozen set.
[[nodiscard]] constexpr bool ThreadSnapshotPassNeedsRetry(
    bool added_thread,
    bool observed_exit_or_recycle_race) noexcept {
    return added_thread || observed_exit_or_recycle_race;
}

class Registry {
public:
    ApplyResult Apply(std::string name,
                      std::uintptr_t address,
                      std::span<const std::uint8_t> expected,
                      std::span<const std::uint8_t> replacement);
    [[nodiscard]] std::size_t checkpoint() const noexcept;
    [[nodiscard]] bool RestoreTo(std::size_t checkpoint);
    [[nodiscard]] bool RestoreAll();
    [[nodiscard]] bool empty() const;

private:
    struct AppliedPatch {
        std::string name;
        std::uintptr_t address = 0;
        std::vector<std::uint8_t> expected;
        std::vector<std::uint8_t> replacement;
        bool mutation_uncertain = false;
        bool protection_restore_pending = false;
        std::uint32_t original_protection = 0;
    };

    std::vector<AppliedPatch> applied_;
};

[[nodiscard]] bool MatchesBytes(std::uintptr_t address,
                                std::span<const std::uint8_t> expected);

}  // namespace spatch::runtime_patch
