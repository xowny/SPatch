#include "CutContentProbe.h"

#include "Logger.h"
#include "RuntimePatch.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>

namespace spatch::cut_content {
namespace {

#if !defined(SPATCH_FINAL_RELEASE)

constexpr std::uintptr_t kLegacyProgressionFindRva = 0x004A0380;
constexpr std::uintptr_t kLatestSteamProgressionFindRva = 0x004A04C0;
constexpr std::uintptr_t kLegacyScriptGetClassRva = 0x00117560;
constexpr std::uintptr_t kLatestSteamScriptGetClassRva = 0x00117390;
constexpr std::uintptr_t kLegacyForceSliceChangeRva = 0x004A1390;
constexpr std::uintptr_t kLatestSteamForceSliceChangeRva = 0x004A14D0;
constexpr std::uintptr_t kLegacyTreeAddRva = 0x001652A0;
constexpr std::uintptr_t kLatestSteamTreeAddRva = 0x001652D0;
constexpr std::uintptr_t kLegacyTreeRemoveRva = 0x0017A0F0;
constexpr std::uintptr_t kLatestSteamTreeRemoveRva = 0x0017A170;
constexpr std::uintptr_t kLegacySliceSetEnabledRva = 0x004BC130;
constexpr std::uintptr_t kLatestSteamSliceSetEnabledRva = 0x004BC200;
constexpr std::uintptr_t kProgressionTrackerRva = 0x0240A0E0;
constexpr std::uintptr_t kGameSlicesOffset = 0x20;
constexpr std::uintptr_t kContainerGameSlicesOffset = 0x68;
constexpr std::uintptr_t kDisabledGameSlicesOffset = 0xB0;
constexpr std::uint64_t kProbeIntervalFrames = 300;
constexpr std::uint64_t kForceObservationIntervalFrames = 120;
constexpr std::uint32_t kMaximumReadinessAttempts = 120;
constexpr std::uint32_t kMaximumForceObservations = 60;

constexpr std::array<std::uint8_t, 24> kProgressionFindSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9,
    0x45, 0x84, 0xC0, 0x74, 0x17, 0x8B, 0x12, 0x85};

constexpr std::array<std::uint8_t, 16> kScriptGetClassSignature = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xD1, 0x48,
    0x8D, 0x4C, 0x24, 0x38, 0x41, 0xB9, 0x01, 0x00};

constexpr std::array<std::uint8_t, 16> kForceSliceChangeSignature = {
    0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
    0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE};

constexpr std::array<std::uint8_t, 19> kTreeAddSignature = {
    0x48, 0x85, 0xD2, 0x0F, 0x84, 0x25, 0x02, 0x00, 0x00, 0x48,
    0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};

constexpr std::array<std::uint8_t, 19> kTreeRemoveSignature = {
    0x48, 0x85, 0xD2, 0x0F, 0x84, 0x60, 0x01, 0x00, 0x00, 0x48,
    0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};

constexpr std::array<std::uint8_t, 26> kSliceSetEnabledSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24,
    0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x8B, 0xA9, 0x88, 0x01, 0x00, 0x00};

struct Candidate {
    const char* name;
    std::uint32_t uid;
};

constexpr std::array<Candidate, 3> kReadinessControls = {{
    {"M_NMC", SymbolHash("M_NMC")},
    {"M_BSL", SymbolHash("M_BSL")},
    {"M_TBH", SymbolHash("M_TBH")},
}};

constexpr std::array<Candidate, 17> kCandidates = {{
    {"M_CO", SymbolHash("M_CO")},
    {"CO_01_StealTanker", SymbolHash("CO_01_StealTanker")},
    {"CO_02_DriveToElection", SymbolHash("CO_02_DriveToElection")},
    {"CO_03_ElectionBattle01", SymbolHash("CO_03_ElectionBattle01")},
    {"CO_04_ElectionBattle02", SymbolHash("CO_04_ElectionBattle02")},
    {"CO_05_RailShooter01", SymbolHash("CO_05_RailShooter01")},
    {"CO_06_RailDriver", SymbolHash("CO_06_RailDriver")},
    {"CO_07_RailShooter02", SymbolHash("CO_07_RailShooter02")},
    {"CO_08_Finale", SymbolHash("CO_08_Finale")},
    {"E_TH", SymbolHash("E_TH")},
    {"E_TH_1", SymbolHash("E_TH_1")},
    {"E_DTC", SymbolHash("E_DTC")},
    {"E_DTC_1", SymbolHash("E_DTC_1")},
    {"E_BrokenNoseCall", SymbolHash("E_BrokenNoseCall")},
    {"M_RL", SymbolHash("M_RL")},
    {"M_AFS", SymbolHash("M_AFS")},
    {"M_BNJ", SymbolHash("M_BNJ")},
}};

constexpr std::size_t kMCoSliceCount = 9;

struct ScriptCandidate {
    const char* name;
    bool required_for_m_co;
};

constexpr std::array<ScriptCandidate, 13> kScriptClasses = {{
    {"COMission", true},
    {"CO_01_StealTanker", true},
    {"CO_02_DriveToElection", true},
    {"CO_03_ElectionBattle01", true},
    {"CO_04_ElectionBattle02", true},
    {"CO_05_RailShooter01", true},
    {"CO_06_RailDriver", true},
    {"CO_07_RailShooter02", true},
    {"CO_08_Finale", true},
    {"E_TH", false},
    {"E_TH_1", false},
    {"E_DTC", false},
    {"E_DTC_1", false},
}};

static_assert(SymbolHash("E_BrokenNoseCall") == 0x3679F77Cu);
static_assert(SymbolHash("M_CO") == 0x106E37DEu);
static_assert(SymbolHash("COMission") == 0x0187AF31u);
static_assert(SymbolHash("ProgressionTriggers_Missions-COMission") == 0x8BB32A24u);
static_assert(SymbolHash("COMission-Restore_00") == 0x71396DE4u);
static_assert(SymbolHash("EndLocations-M_CO") == 0xB12509ADu);
static_assert(SymbolHash("E_TH") == 0xD05D1AB8u);
static_assert(SymbolHash("E_DTC") == 0xB7F394DFu);

using ProgressionFindFn = void* (*)(void* tracker,
                                    const std::uint32_t* symbol,
                                    bool search_disabled_slices);
using ScriptGetClassFn = void* (*)(const char* class_name);
using ForceSliceChangeFn = void (*)(void* tracker,
                                    void* game_slice,
                                    bool simulate_rewards);
using TreeMutationFn = void (*)(void* tree, void* node);
using SliceSetEnabledFn = void (*)(void* game_slice, bool enabled);

struct SliceSnapshot {
    std::uint32_t uid = 0;
    std::uint32_t layer_uid = 0;
    std::uint32_t trigger_uid = 0;
    std::uint32_t restore_marker_uid = 0;
    std::uint32_t completion_marker_uid = 0;
    std::uint32_t state = 0;
    std::uint32_t type = 0;
    void* script = nullptr;
    void* scene_settings = nullptr;
    void* parent = nullptr;
    std::uint32_t child_count = 0;
    void** children = nullptr;
    std::array<void*, 8> child_entries{};
    std::uint8_t enabled = 0;
    std::uint8_t root = 0;
    std::uint8_t dirty = 0;
    std::array<char, 64> name{};
    std::array<char, 64> script_class_name{};
};

std::atomic<bool> g_enabled = false;
std::atomic<std::uint32_t> g_active_frame_calls = 0;
std::atomic<std::uint64_t> g_frame_count = 0;
std::atomic<std::uint32_t> g_attempt_count = 0;
std::atomic<bool> g_force_pending = false;
std::atomic<bool> g_force_observing = false;
std::atomic<std::uint32_t> g_force_observation_count = 0;
void* g_progression_tracker = nullptr;
ProgressionFindFn g_progression_find = nullptr;
ScriptGetClassFn g_script_get_class = nullptr;
ForceSliceChangeFn g_force_slice_change = nullptr;
TreeMutationFn g_tree_add = nullptr;
TreeMutationFn g_tree_remove = nullptr;
SliceSetEnabledFn g_slice_set_enabled = nullptr;
void* g_force_target = nullptr;
bool g_force_requested = false;
std::size_t g_force_target_index = 0;
const char* g_force_target_name = "M_CO";

class FrameCallGuard {
public:
    FrameCallGuard() noexcept {
        if (!g_enabled.load(std::memory_order_acquire)) {
            return;
        }
        g_active_frame_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!g_enabled.load(std::memory_order_acquire)) {
            g_active_frame_calls.fetch_sub(1, std::memory_order_release);
            return;
        }
        accepted_ = true;
    }

    FrameCallGuard(const FrameCallGuard&) = delete;
    FrameCallGuard& operator=(const FrameCallGuard&) = delete;

    ~FrameCallGuard() {
        if (accepted_) {
            g_active_frame_calls.fetch_sub(1, std::memory_order_release);
        }
    }

    [[nodiscard]] bool accepted() const noexcept { return accepted_; }

private:
    bool accepted_ = false;
};

bool ProbeRequested(const std::filesystem::path& trigger_path) noexcept {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        L"SPATCH_CUT_CONTENT_PROBE", value, static_cast<DWORD>(std::size(value)));
    if (length != 0 && length < std::size(value)) {
        wchar_t* end = nullptr;
        if (std::wcstol(value, &end, 10) != 0 && end != value) {
            return true;
        }
    }
    std::error_code error;
    return !trigger_path.empty() && std::filesystem::is_regular_file(trigger_path, error);
}

void* SafeFind(const std::uint32_t uid,
               const bool search_disabled_slices,
               bool& call_succeeded) noexcept {
    call_succeeded = false;
    void* result = nullptr;
    __try {
        result = g_progression_find(
            g_progression_tracker, &uid, search_disabled_slices);
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

void* SafeGetScriptClass(const char* name, bool& call_succeeded) noexcept {
    call_succeeded = false;
    void* result = nullptr;
    __try {
        result = g_script_get_class(name);
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

bool SafeCopyString(const char* source,
                    char* output,
                    const std::size_t output_size) noexcept {
    if (source == nullptr || output == nullptr || output_size == 0) {
        return false;
    }
    output[0] = '\0';
    bool terminated = false;
    __try {
        for (std::size_t position = 0; position + 1 < output_size; ++position) {
            output[position] = source[position];
            if (source[position] == '\0') {
                terminated = true;
                break;
            }
        }
        if (!terminated) {
            output[output_size - 1] = '\0';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        terminated = false;
    }
    return terminated;
}

bool SafeReadSliceSnapshot(void* slice, SliceSnapshot& snapshot) noexcept {
    snapshot = {};
    if (slice == nullptr) {
        return false;
    }

    const char* name = nullptr;
    const char* script_class_name = nullptr;
    __try {
        const auto base = reinterpret_cast<std::uintptr_t>(slice);
        snapshot.uid = *reinterpret_cast<const std::uint32_t*>(base + 0x18);
        name = *reinterpret_cast<const char* const*>(base + 0x20);
        script_class_name = *reinterpret_cast<const char* const*>(base + 0x30);
        snapshot.layer_uid = *reinterpret_cast<const std::uint32_t*>(base + 0xA8);
        snapshot.trigger_uid = *reinterpret_cast<const std::uint32_t*>(base + 0xAC);
        snapshot.restore_marker_uid =
            *reinterpret_cast<const std::uint32_t*>(base + 0xB0);
        snapshot.completion_marker_uid =
            *reinterpret_cast<const std::uint32_t*>(base + 0xC8);
        snapshot.script = *reinterpret_cast<void* const*>(base + 0xF8);
        snapshot.state = *reinterpret_cast<const std::uint32_t*>(base + 0x108);
        snapshot.type = *reinterpret_cast<const std::uint32_t*>(base + 0x10C);
        snapshot.enabled = *reinterpret_cast<const std::uint8_t*>(base + 0x140);
        snapshot.root = *reinterpret_cast<const std::uint8_t*>(base + 0x141);
        snapshot.dirty = *reinterpret_cast<const std::uint8_t*>(base + 0x142);
        snapshot.scene_settings = *reinterpret_cast<void* const*>(base + 0x160);
        snapshot.parent = *reinterpret_cast<void* const*>(base + 0x170);
        snapshot.child_count =
            *reinterpret_cast<const std::uint32_t*>(base + 0x188);
        snapshot.children = *reinterpret_cast<void***>(base + 0x190);
        if (snapshot.children != nullptr) {
            for (std::size_t index = 0;
                 index < snapshot.child_entries.size() &&
                 index < snapshot.child_count;
                 ++index) {
                snapshot.child_entries[index] = snapshot.children[index];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snapshot = {};
        return false;
    }

    return SafeCopyString(name, snapshot.name.data(), snapshot.name.size()) &&
           SafeCopyString(script_class_name,
                          snapshot.script_class_name.data(),
                          snapshot.script_class_name.size());
}

void* SafeReadActiveMaster(bool& read_succeeded) noexcept {
    read_succeeded = false;
    void* active_master = nullptr;
    __try {
        const auto base = reinterpret_cast<std::uintptr_t>(g_progression_tracker);
        active_master = *reinterpret_cast<void* const*>(base + 0x15F0);
        read_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active_master = nullptr;
    }
    return active_master;
}

void* SafeReadLastCheckpoint(bool& read_succeeded) noexcept {
    read_succeeded = false;
    void* checkpoint = nullptr;
    __try {
        const auto base = reinterpret_cast<std::uintptr_t>(g_progression_tracker);
        checkpoint = *reinterpret_cast<void* const*>(base + 0x15F8);
        read_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        checkpoint = nullptr;
    }
    return checkpoint;
}

bool SafeReadTrackerState(std::uint32_t& state) noexcept {
    state = 0;
    __try {
        const auto base = reinterpret_cast<std::uintptr_t>(g_progression_tracker);
        state = *reinterpret_cast<const std::uint32_t*>(base + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        state = 0;
        return false;
    }
}

bool SafeForceSliceChange(void* slice) noexcept {
    bool call_succeeded = false;
    __try {
        g_force_slice_change(g_progression_tracker, slice, false);
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        call_succeeded = false;
    }
    return call_succeeded;
}

bool SafeTreeMutation(TreeMutationFn operation, void* tree, void* slice) noexcept {
    bool call_succeeded = false;
    __try {
        operation(tree, slice);
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        call_succeeded = false;
    }
    return call_succeeded;
}

bool SafeSetEnabled(void* slice, const bool enabled) noexcept {
    bool call_succeeded = false;
    __try {
        g_slice_set_enabled(slice, enabled);
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        call_succeeded = false;
    }
    return call_succeeded;
}

enum class MoveSliceResult : std::uint8_t {
    source_unchanged,
    moved,
    detached,
};

MoveSliceResult SafeMoveSlice(
    void* source_tree, void* destination_tree, void* slice) noexcept {
    if (!SafeTreeMutation(g_tree_remove, source_tree, slice)) {
        return MoveSliceResult::source_unchanged;
    }
    if (SafeTreeMutation(g_tree_add, destination_tree, slice)) {
        return MoveSliceResult::moved;
    }
    return SafeTreeMutation(g_tree_add, source_tree, slice)
        ? MoveSliceResult::source_unchanged
        : MoveSliceResult::detached;
}

bool SafeReadTreeCounts(int& live, int& containers, int& disabled) noexcept {
    live = 0;
    containers = 0;
    disabled = 0;
    __try {
        const auto base = reinterpret_cast<std::uintptr_t>(g_progression_tracker);
        live = *reinterpret_cast<const int*>(base + 0x60);
        containers = *reinterpret_cast<const int*>(base + 0xA8);
        disabled = *reinterpret_cast<const int*>(base + 0xF0);
        return live >= 0 && containers >= 0 && disabled >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        live = 0;
        containers = 0;
        disabled = 0;
        return false;
    }
}

void DisableAfterCallFailure(const char* operation, const char* name) noexcept {
    log::ErrorF("cut_content_probe_failed operation=%s name=%s reason=engine_call_fault",
                operation,
                name);
    g_enabled.store(false, std::memory_order_release);
}

bool IsProgressionReady() noexcept {
    for (const Candidate& control : kReadinessControls) {
        bool succeeded = false;
        if (SafeFind(control.uid, false, succeeded) != nullptr) {
            return true;
        }
        if (!succeeded) {
            DisableAfterCallFailure("progression_find_live", control.name);
            return false;
        }
    }
    return false;
}

void LogSliceSnapshot(const char* phase, void* slice) noexcept {
    SliceSnapshot snapshot{};
    const bool valid = SafeReadSliceSnapshot(slice, snapshot);
    log::InfoF("cut_content_slice_snapshot phase=%s valid=%d slice=0x%p uid=0x%08X "
               "name=%s script_class=%s layer=0x%08X trigger=0x%08X "
               "restore=0x%08X complete=0x%08X state=%u type=%u script=0x%p "
               "scene_settings=0x%p parent=0x%p children=%u enabled=%u root=%u "
               "dirty=%u",
               phase,
               valid ? 1 : 0,
               slice,
               snapshot.uid,
               valid ? snapshot.name.data() : "<invalid>",
               valid ? snapshot.script_class_name.data() : "<invalid>",
               snapshot.layer_uid,
               snapshot.trigger_uid,
               snapshot.restore_marker_uid,
               snapshot.completion_marker_uid,
               snapshot.state,
               snapshot.type,
               snapshot.script,
               snapshot.scene_settings,
               snapshot.parent,
               snapshot.child_count,
               snapshot.enabled,
               snapshot.root,
               snapshot.dirty);
}

bool IsValidatedMCoTarget(void* slice) noexcept {
    SliceSnapshot snapshot{};
    if (!SafeReadSliceSnapshot(slice, snapshot)) {
        return false;
    }
    return snapshot.uid == SymbolHash("M_CO") &&
           std::strcmp(snapshot.name.data(), "M_CO") == 0 &&
           std::strcmp(snapshot.script_class_name.data(), "COMission") == 0 &&
           snapshot.layer_uid == SymbolHash("COMission") &&
           snapshot.trigger_uid ==
               SymbolHash("ProgressionTriggers_Missions-COMission") &&
           snapshot.restore_marker_uid == SymbolHash("COMission-Restore_00") &&
           snapshot.completion_marker_uid == SymbolHash("EndLocations-M_CO") &&
           snapshot.type == 11 && snapshot.parent == nullptr &&
           snapshot.child_count == 8 && snapshot.children != nullptr;
}

bool IsValidatedMCoSet(
    const std::array<void*, kMCoSliceCount>& slices) noexcept {
    if (!IsValidatedMCoTarget(slices[0])) {
        return false;
    }

    SliceSnapshot root{};
    if (!SafeReadSliceSnapshot(slices[0], root)) {
        return false;
    }
    for (std::size_t index = 1; index < slices.size(); ++index) {
        SliceSnapshot child{};
        if (!SafeReadSliceSnapshot(slices[index], child) ||
            child.uid != kCandidates[index].uid ||
            std::strcmp(child.name.data(), kCandidates[index].name) != 0 ||
            std::strcmp(child.script_class_name.data(), kCandidates[index].name) != 0 ||
            child.layer_uid != kCandidates[index].uid || child.type != 11 ||
            child.parent != slices[0] || child.child_count != 0) {
            return false;
        }
        if (root.child_entries[index - 1] != slices[index]) {
            return false;
        }
    }
    return true;
}

bool VerifyMCoPromotion(
    const std::array<void*, kMCoSliceCount>& slices) noexcept {
    for (std::size_t index = 0; index < slices.size(); ++index) {
        bool live_find_succeeded = false;
        void* live =
            SafeFind(kCandidates[index].uid, false, live_find_succeeded);
        if (!live_find_succeeded || live != slices[index]) {
            return false;
        }
        SliceSnapshot snapshot{};
        if (!SafeReadSliceSnapshot(slices[index], snapshot) ||
            snapshot.enabled == 0) {
            return false;
        }
    }
    return true;
}

bool PromoteMCo(const std::array<void*, kMCoSliceCount>& slices) noexcept {
    enum class SliceLocation : std::uint8_t {
        disabled,
        live,
        container,
        detached,
    };
    int live_before = 0;
    int containers_before = 0;
    int disabled_before = 0;
    const bool counts_before_valid =
        SafeReadTreeCounts(live_before, containers_before, disabled_before);

    if (!SafeSetEnabled(slices[0], true)) {
        DisableAfterCallFailure("slice_set_enabled", "M_CO");
        return false;
    }

    const auto tracker = reinterpret_cast<std::uintptr_t>(g_progression_tracker);
    void* disabled_tree =
        reinterpret_cast<void*>(tracker + kDisabledGameSlicesOffset);
    void* live_tree = reinterpret_cast<void*>(tracker + kGameSlicesOffset);
    void* container_tree =
        reinterpret_cast<void*>(tracker + kContainerGameSlicesOffset);
    std::array<SliceLocation, kMCoSliceCount> locations{};

    for (std::size_t index = 1; index < slices.size(); ++index) {
        const MoveSliceResult result =
            SafeMoveSlice(disabled_tree, live_tree, slices[index]);
        if (result != MoveSliceResult::moved) {
            if (result == MoveSliceResult::detached) {
                locations[index] = SliceLocation::detached;
                log::ErrorF("cut_content_slice_detached operation=tree_promote name=%s",
                            kCandidates[index].name);
            }
            DisableAfterCallFailure("tree_promote", kCandidates[index].name);
            break;
        }
        locations[index] = SliceLocation::live;
    }
    if (g_enabled.load(std::memory_order_acquire)) {
        const MoveSliceResult result =
            SafeMoveSlice(disabled_tree, container_tree, slices[0]);
        if (result == MoveSliceResult::moved) {
            locations[0] = SliceLocation::container;
        } else {
            if (result == MoveSliceResult::detached) {
                locations[0] = SliceLocation::detached;
                log::Error("cut_content_slice_detached operation=tree_promote name=M_CO");
            }
            DisableAfterCallFailure("tree_promote", "M_CO");
        }
    }

    const bool promotion_valid =
        g_enabled.load(std::memory_order_acquire) && VerifyMCoPromotion(slices);
    if (!promotion_valid) {
        bool rollback_complete = true;
        for (std::size_t reverse = slices.size(); reverse-- > 0;) {
            MoveSliceResult result = MoveSliceResult::source_unchanged;
            switch (locations[reverse]) {
                case SliceLocation::disabled:
                    continue;
                case SliceLocation::live:
                    result = SafeMoveSlice(live_tree, disabled_tree, slices[reverse]);
                    break;
                case SliceLocation::container:
                    result = SafeMoveSlice(
                        container_tree, disabled_tree, slices[reverse]);
                    break;
                case SliceLocation::detached:
                    result = SafeTreeMutation(
                        g_tree_add, disabled_tree, slices[reverse])
                        ? MoveSliceResult::moved
                        : MoveSliceResult::detached;
                    break;
            }
            if (result != MoveSliceResult::moved) {
                rollback_complete = false;
                log::ErrorF(
                    "cut_content_rollback_failed name=%s detached=%d",
                    kCandidates[reverse].name,
                    result == MoveSliceResult::detached ? 1 : 0);
            }
        }
        rollback_complete = SafeSetEnabled(slices[0], false) && rollback_complete;
        log::ErrorF(
            "cut_content_promotion_failed name=M_CO rollback_attempted=1 rollback_complete=%d",
            rollback_complete ? 1 : 0);
        g_enabled.store(false, std::memory_order_release);
        return false;
    }

    int live_after = 0;
    int containers_after = 0;
    int disabled_after = 0;
    const bool counts_after_valid =
        SafeReadTreeCounts(live_after, containers_after, disabled_after);
    log::InfoF("cut_content_promotion_complete name=M_CO counts_before_valid=%d "
               "live_before=%d containers_before=%d disabled_before=%d "
               "counts_after_valid=%d live_after=%d containers_after=%d "
               "disabled_after=%d",
               counts_before_valid ? 1 : 0,
               live_before,
               containers_before,
               disabled_before,
               counts_after_valid ? 1 : 0,
               live_after,
               containers_after,
               disabled_after);
    return true;
}

void RunForce() noexcept {
    log::InfoF("cut_content_force_begin name=%s target=0x%p simulate_rewards=0",
               g_force_target_name,
               g_force_target);
    LogSliceSnapshot("before_force", g_force_target);
    if (!SafeForceSliceChange(g_force_target)) {
        DisableAfterCallFailure("force_slice_change", g_force_target_name);
        return;
    }

    log::InfoF("cut_content_force_returned name=%s", g_force_target_name);
    LogSliceSnapshot("after_force_return", g_force_target);
    g_frame_count.store(0, std::memory_order_relaxed);
    g_force_observation_count.store(0, std::memory_order_relaxed);
    g_force_observing.store(true, std::memory_order_release);
}

void ObserveForce() noexcept {
    const std::uint32_t observation =
        g_force_observation_count.fetch_add(1, std::memory_order_relaxed) + 1;
    bool active_master_read = false;
    void* active_master = SafeReadActiveMaster(active_master_read);
    bool checkpoint_read = false;
    void* checkpoint = SafeReadLastCheckpoint(checkpoint_read);
    std::uint32_t tracker_state = 0;
    const bool tracker_state_read = SafeReadTrackerState(tracker_state);
    bool root_find_succeeded = false;
    void* root = SafeFind(SymbolHash("M_CO"), true, root_find_succeeded);
    bool first_child_find_succeeded = false;
    void* first_child = SafeFind(SymbolHash("CO_01_StealTanker"),
                                 true,
                                 first_child_find_succeeded);
    log::InfoF("cut_content_force_observation sample=%u tracker_state_read=%d "
               "tracker_state=%u active_master_read=%d active_master=0x%p "
               "checkpoint_read=%d checkpoint=0x%p root_find=%d root=0x%p "
               "first_child_find=%d first_child=0x%p",
               observation,
               tracker_state_read ? 1 : 0,
               tracker_state,
               active_master_read ? 1 : 0,
               active_master,
               checkpoint_read ? 1 : 0,
               checkpoint,
               root_find_succeeded ? 1 : 0,
               root,
               first_child_find_succeeded ? 1 : 0,
               first_child);
    LogSliceSnapshot("observe_active_master", active_master);
    LogSliceSnapshot("observe_checkpoint", checkpoint);
    LogSliceSnapshot("observe_root", root);
    LogSliceSnapshot("observe_first_child", first_child);

    if (!tracker_state_read || !active_master_read || !checkpoint_read ||
        !root_find_succeeded ||
        !first_child_find_succeeded || observation >= kMaximumForceObservations) {
        log::InfoF("cut_content_force_observation_complete samples=%u", observation);
        g_force_observing.store(false, std::memory_order_release);
        g_enabled.store(false, std::memory_order_release);
    }
}

void RunProbe() noexcept {
    int live_count = 0;
    int container_count = 0;
    int disabled_count = 0;
    const bool counts_valid =
        SafeReadTreeCounts(live_count, container_count, disabled_count);
    log::InfoF("cut_content_probe_ready attempt=%u tree_counts_valid=%d live=%d "
               "containers=%d disabled=%d",
               g_attempt_count.load(std::memory_order_relaxed),
               counts_valid ? 1 : 0,
               live_count,
               container_count,
               disabled_count);

    std::array<void*, kMCoSliceCount> m_co_slices{};
    bool all_m_co_slices_disabled = true;
    for (std::size_t candidate_index = 0;
         candidate_index < kCandidates.size();
         ++candidate_index) {
        const Candidate& candidate = kCandidates[candidate_index];
        bool live_call_succeeded = false;
        void* live = SafeFind(candidate.uid, false, live_call_succeeded);
        if (!live_call_succeeded) {
            DisableAfterCallFailure("progression_find_live", candidate.name);
            return;
        }

        bool disabled_call_succeeded = false;
        void* any = SafeFind(candidate.uid, true, disabled_call_succeeded);
        if (!disabled_call_succeeded) {
            DisableAfterCallFailure("progression_find_disabled", candidate.name);
            return;
        }

        const char* classification =
            live != nullptr ? "live" : (any != nullptr ? "disabled" : "missing");
        log::InfoF("cut_content_probe name=%s uid=0x%08X classification=%s "
                   "live=0x%p any=0x%p",
                   candidate.name,
                   candidate.uid,
                   classification,
                   live,
                   any);
        if (candidate_index < m_co_slices.size()) {
            m_co_slices[candidate_index] = any;
            all_m_co_slices_disabled =
                all_m_co_slices_disabled && live == nullptr && any != nullptr;
        }
    }

    bool required_classes_registered = true;
    for (const ScriptCandidate& candidate : kScriptClasses) {
        bool call_succeeded = false;
        void* script_class = SafeGetScriptClass(candidate.name, call_succeeded);
        if (!call_succeeded) {
            DisableAfterCallFailure("script_get_class", candidate.name);
            return;
        }
        log::InfoF("cut_content_script_class name=%s classification=%s class=0x%p",
                   candidate.name,
                   script_class != nullptr ? "registered" : "missing",
                   script_class);
        if (candidate.required_for_m_co && script_class == nullptr) {
            required_classes_registered = false;
        }
    }

    LogSliceSnapshot("probe", m_co_slices[0]);
    if (g_force_requested) {
        const bool target_set_valid = IsValidatedMCoSet(m_co_slices);
        if (!all_m_co_slices_disabled || !required_classes_registered ||
            !target_set_valid) {
            log::ErrorF("cut_content_force_refused name=M_CO all_slices_disabled=%d "
                        "required_classes_registered=%d target_set_valid=%d target=0x%p",
                        all_m_co_slices_disabled ? 1 : 0,
                        required_classes_registered ? 1 : 0,
                        target_set_valid ? 1 : 0,
                        m_co_slices[0]);
            g_enabled.store(false, std::memory_order_release);
            return;
        }
        if (!PromoteMCo(m_co_slices)) {
            return;
        }
        g_force_target = m_co_slices[g_force_target_index];
        g_force_target_name = kCandidates[g_force_target_index].name;
        g_force_pending.store(true, std::memory_order_release);
        log::InfoF("cut_content_force_scheduled name=%s target=0x%p next_frame=1",
                   g_force_target_name,
                   g_force_target);
        return;
    }

    log::Info("cut_content_probe_complete");
    g_enabled.store(false, std::memory_order_release);
}

bool DrainFrameCalls() noexcept {
    constexpr ULONGLONG kDrainTimeoutMilliseconds = 5000;
    const ULONGLONG deadline = GetTickCount64() + kDrainTimeoutMilliseconds;
    while (g_active_frame_calls.load(std::memory_order_acquire) != 0) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        if (!SwitchToThread()) {
            Sleep(1);
        }
    }
    return true;
}

#endif

}  // namespace

void Initialize(const std::uintptr_t module_base,
                const bool latest_steam_layout,
                const std::filesystem::path& trigger_path) noexcept {
#if !defined(SPATCH_FINAL_RELEASE)
    Shutdown();
    if (g_active_frame_calls.load(std::memory_order_acquire) != 0) {
        log::Error("cut_content_probe_disabled reason=previous_callback_not_drained");
        return;
    }
    std::filesystem::path force_trigger_path;
    std::size_t force_target_index = 0;
    bool force_requested = false;
    try {
        for (std::size_t index = 0; index < kMCoSliceCount; ++index) {
            std::wstring filename = L"SPatch.cutcontent.force-";
            for (const unsigned char character :
                 std::string_view(kCandidates[index].name)) {
                filename.push_back(static_cast<wchar_t>(character));
            }
            const std::filesystem::path candidate_path =
                trigger_path.parent_path() / filename;
            std::error_code error;
            if (!std::filesystem::is_regular_file(candidate_path, error)) {
                continue;
            }
            if (force_requested) {
                log::Error("cut_content_probe_disabled reason=multiple_force_triggers");
                return;
            }
            force_requested = true;
            force_target_index = index;
            force_trigger_path = candidate_path;
        }
    } catch (...) {
        log::Error("cut_content_probe_disabled reason=force_trigger_path_error");
        return;
    }
    const bool probe_requested = ProbeRequested(trigger_path);
    if ((!probe_requested && !force_requested) || module_base == 0) {
        return;
    }

    const std::uintptr_t find_rva = latest_steam_layout
                                        ? kLatestSteamProgressionFindRva
                                        : kLegacyProgressionFindRva;
    const std::uintptr_t script_get_class_rva = latest_steam_layout
                                                    ? kLatestSteamScriptGetClassRva
                                                    : kLegacyScriptGetClassRva;
    const std::uintptr_t force_slice_change_rva = latest_steam_layout
                                                      ? kLatestSteamForceSliceChangeRva
                                                      : kLegacyForceSliceChangeRva;
    const std::uintptr_t tree_add_rva = latest_steam_layout
                                            ? kLatestSteamTreeAddRva
                                            : kLegacyTreeAddRva;
    const std::uintptr_t tree_remove_rva = latest_steam_layout
                                               ? kLatestSteamTreeRemoveRva
                                               : kLegacyTreeRemoveRva;
    const std::uintptr_t slice_set_enabled_rva = latest_steam_layout
                                                     ? kLatestSteamSliceSetEnabledRva
                                                     : kLegacySliceSetEnabledRva;
    const std::uintptr_t find_address = module_base + find_rva;
    const std::uintptr_t script_get_class_address = module_base + script_get_class_rva;
    const std::uintptr_t force_slice_change_address =
        module_base + force_slice_change_rva;
    const std::uintptr_t tree_add_address = module_base + tree_add_rva;
    const std::uintptr_t tree_remove_address = module_base + tree_remove_rva;
    const std::uintptr_t slice_set_enabled_address =
        module_base + slice_set_enabled_rva;
    if (!runtime_patch::MatchesBytes(find_address, kProgressionFindSignature)) {
        log::ErrorF("cut_content_probe_disabled reason=find_signature_mismatch target=0x%p",
                    reinterpret_cast<void*>(find_address));
        return;
    }
    if (force_requested &&
        !runtime_patch::MatchesBytes(tree_add_address, kTreeAddSignature)) {
        log::ErrorF("cut_content_probe_disabled reason=tree_add_signature_mismatch "
                    "target=0x%p",
                    reinterpret_cast<void*>(tree_add_address));
        return;
    }
    if (force_requested &&
        !runtime_patch::MatchesBytes(tree_remove_address, kTreeRemoveSignature)) {
        log::ErrorF("cut_content_probe_disabled reason=tree_remove_signature_mismatch "
                    "target=0x%p",
                    reinterpret_cast<void*>(tree_remove_address));
        return;
    }
    if (force_requested &&
        !runtime_patch::MatchesBytes(slice_set_enabled_address,
                                     kSliceSetEnabledSignature)) {
        log::ErrorF("cut_content_probe_disabled reason=slice_set_enabled_signature_mismatch "
                    "target=0x%p",
                    reinterpret_cast<void*>(slice_set_enabled_address));
        return;
    }
    if (!runtime_patch::MatchesBytes(script_get_class_address,
                                     kScriptGetClassSignature)) {
        log::ErrorF("cut_content_probe_disabled reason=script_get_class_signature_mismatch "
                    "target=0x%p",
                    reinterpret_cast<void*>(script_get_class_address));
        return;
    }
    if (force_requested &&
        !runtime_patch::MatchesBytes(force_slice_change_address,
                                     kForceSliceChangeSignature)) {
        log::ErrorF("cut_content_probe_disabled reason=force_slice_change_signature_mismatch "
                    "target=0x%p",
                    reinterpret_cast<void*>(force_slice_change_address));
        return;
    }

    g_progression_tracker =
        reinterpret_cast<void*>(module_base + kProgressionTrackerRva);
    g_progression_find = reinterpret_cast<ProgressionFindFn>(find_address);
    g_script_get_class =
        reinterpret_cast<ScriptGetClassFn>(script_get_class_address);
    g_force_slice_change =
        reinterpret_cast<ForceSliceChangeFn>(force_slice_change_address);
    g_tree_add = reinterpret_cast<TreeMutationFn>(tree_add_address);
    g_tree_remove = reinterpret_cast<TreeMutationFn>(tree_remove_address);
    g_slice_set_enabled =
        reinterpret_cast<SliceSetEnabledFn>(slice_set_enabled_address);
    g_force_requested = force_requested;
    g_force_target_index = force_target_index;
    g_force_target_name = kCandidates[force_target_index].name;
    if (force_requested && !DeleteFileW(force_trigger_path.c_str())) {
        log::ErrorF("cut_content_probe_disabled reason=force_trigger_consume_failed "
                    "win32=%lu",
                    GetLastError());
        Shutdown();
        return;
    }
    g_enabled.store(true, std::memory_order_release);
    log::InfoF("cut_content_probe_armed layout=%s tracker=0x%p find=0x%p "
               "script_get_class=0x%p force_requested=%d force_target=%s "
               "force_slice_change=0x%p "
               "tree_add=0x%p tree_remove=0x%p slice_set_enabled=0x%p",
               latest_steam_layout ? "latest_steam" : "legacy_researched",
               g_progression_tracker,
               reinterpret_cast<void*>(find_address),
               reinterpret_cast<void*>(script_get_class_address),
               force_requested ? 1 : 0,
               g_force_target_name,
               reinterpret_cast<void*>(force_slice_change_address),
               reinterpret_cast<void*>(tree_add_address),
               reinterpret_cast<void*>(tree_remove_address),
               reinterpret_cast<void*>(slice_set_enabled_address));
#else
    (void)module_base;
    (void)latest_steam_layout;
    (void)trigger_path;
#endif
}

void OnGameThreadFrame() noexcept {
#if !defined(SPATCH_FINAL_RELEASE)
    FrameCallGuard frame_call;
    if (!frame_call.accepted()) {
        return;
    }

    if (g_force_pending.exchange(false, std::memory_order_acq_rel)) {
        RunForce();
        return;
    }

    const std::uint64_t frame =
        g_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_force_observing.load(std::memory_order_acquire)) {
        if (frame % kForceObservationIntervalFrames == 0) {
            ObserveForce();
        }
        return;
    }
    if (frame % kProbeIntervalFrames != 0) {
        return;
    }

    const std::uint32_t attempt =
        g_attempt_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!IsProgressionReady()) {
        if (g_enabled.load(std::memory_order_acquire) &&
            attempt >= kMaximumReadinessAttempts) {
            log::WarnF("cut_content_probe_timeout attempts=%u reason=progression_not_ready",
                       attempt);
            g_enabled.store(false, std::memory_order_release);
        }
        return;
    }
    RunProbe();
#endif
}

bool IsArmed() noexcept {
#if !defined(SPATCH_FINAL_RELEASE)
    return g_enabled.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void Shutdown() noexcept {
#if !defined(SPATCH_FINAL_RELEASE)
    g_enabled.store(false, std::memory_order_release);
    if (!DrainFrameCalls()) {
        log::Warn("cut_content_probe_shutdown_deferred reason=active_callback_timeout");
        return;
    }
    g_frame_count.store(0, std::memory_order_relaxed);
    g_attempt_count.store(0, std::memory_order_relaxed);
    g_force_pending.store(false, std::memory_order_relaxed);
    g_force_observing.store(false, std::memory_order_relaxed);
    g_force_observation_count.store(0, std::memory_order_relaxed);
    g_progression_tracker = nullptr;
    g_progression_find = nullptr;
    g_script_get_class = nullptr;
    g_force_slice_change = nullptr;
    g_tree_add = nullptr;
    g_tree_remove = nullptr;
    g_slice_set_enabled = nullptr;
    g_force_target = nullptr;
    g_force_requested = false;
    g_force_target_index = 0;
    g_force_target_name = "M_CO";
#endif
}

}  // namespace spatch::cut_content
