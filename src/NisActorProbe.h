#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace spatch::hooks::nisprobe {

inline constexpr std::size_t kMaxRetainedActorStates = 4096;

enum class RestoreDisposition {
    tracked,
    duplicate,
    never_seen,
};

struct ActorStateRecord {
    bool live = false;
    unsigned int setup_count = 0;
    unsigned int restore_count = 0;
    std::uintptr_t last_target = 0;
    std::uint64_t last_touch = 0;
};

struct Tracker {
    std::unordered_map<std::uintptr_t, ActorStateRecord> states{};
    unsigned long long live_count = 0;
    std::uint64_t touch_sequence = 0;
};

struct SetupResult {
    bool duplicate = false;
    unsigned long long active_count = 0;
    unsigned int setup_count = 0;
    unsigned int restore_count = 0;
    std::uintptr_t last_target = 0;
};

struct RestoreResult {
    RestoreDisposition disposition = RestoreDisposition::never_seen;
    unsigned long long active_count = 0;
    unsigned int setup_count = 0;
    unsigned int restore_count = 0;
    std::uintptr_t last_target = 0;
};

struct RestoreForwardingDecision {
    bool call_original = true;
    bool suppressed_duplicate = false;
};

inline bool MakeRoomForActorState(Tracker& tracker) {
    if (tracker.states.size() < kMaxRetainedActorStates) {
        return true;
    }

    auto oldest_inactive = tracker.states.end();
    std::uint64_t oldest_touch = (std::numeric_limits<std::uint64_t>::max)();
    for (auto it = tracker.states.begin(); it != tracker.states.end(); ++it) {
        if (!it->second.live && it->second.last_touch < oldest_touch) {
            oldest_touch = it->second.last_touch;
            oldest_inactive = it;
        }
    }
    if (oldest_inactive != tracker.states.end()) {
        tracker.states.erase(oldest_inactive);
        return true;
    }
    // Every retained record is still live. Never grow the table without a
    // bound just because a scene temporarily owns more actors than the probe
    // can track; the caller will treat this one setup as untracked.
    return false;
}

inline SetupResult TrackSetup(Tracker& tracker,
                              std::uintptr_t actor_state,
                              std::uintptr_t actor_target) {
    SetupResult result{};
    if (actor_state != 0) {
        auto existing = tracker.states.find(actor_state);
        if (existing == tracker.states.end()) {
            if (!MakeRoomForActorState(tracker)) {
                result.active_count = tracker.live_count;
                return result;
            }
            try {
                existing = tracker.states.try_emplace(actor_state).first;
            } catch (...) {
                // Tracking is diagnostic/optional. A transient allocation
                // failure must not escape an engine detour or terminate the
                // game; leave the actor untracked and preserve the bound.
                result.active_count = tracker.live_count;
                return result;
            }
        }
        auto& record = existing->second;
        result.duplicate = record.live;
        if (!record.live) {
            ++tracker.live_count;
        }
        record.live = true;
        ++record.setup_count;
        record.last_target = actor_target;
        record.last_touch = ++tracker.touch_sequence;
        result.setup_count = record.setup_count;
        result.restore_count = record.restore_count;
        result.last_target = record.last_target;
    }

    result.active_count = tracker.live_count;
    return result;
}

inline RestoreResult TrackRestore(Tracker& tracker, std::uintptr_t actor_state) {
    RestoreResult result{};
    if (actor_state == 0) {
        result.active_count = tracker.live_count;
        return result;
    }

    const auto it = tracker.states.find(actor_state);
    if (it == tracker.states.end()) {
        result.active_count = tracker.live_count;
        return result;
    }

    auto& record = it->second;
    ++record.restore_count;
    record.last_touch = ++tracker.touch_sequence;
    result.setup_count = record.setup_count;
    result.restore_count = record.restore_count;
    result.last_target = record.last_target;

    if (record.live) {
        record.live = false;
        if (tracker.live_count != 0) {
            --tracker.live_count;
        }
        result.disposition = RestoreDisposition::tracked;
    } else {
        result.disposition = RestoreDisposition::duplicate;
    }

    result.active_count = tracker.live_count;
    return result;
}

inline RestoreForwardingDecision ResolveRestoreForwardingDecision(
    RestoreDisposition disposition,
    bool suppress_duplicate_restores) {
    RestoreForwardingDecision decision{};
    if (disposition == RestoreDisposition::duplicate && suppress_duplicate_restores) {
        decision.call_original = false;
        decision.suppressed_duplicate = true;
    }
    return decision;
}

inline void ResetTracker(Tracker& tracker) {
    tracker.states.clear();
    tracker.live_count = 0;
    tracker.touch_sequence = 0;
}

inline const char* DescribeDisposition(RestoreDisposition disposition) {
    switch (disposition) {
        case RestoreDisposition::tracked:
            return "tracked";
        case RestoreDisposition::duplicate:
            return "duplicate";
        case RestoreDisposition::never_seen:
            return "never_seen";
        default:
            return "unknown";
    }
}

}  // namespace spatch::hooks::nisprobe
