#include "ShenLongComponent.hpp"

#include "ShenLongNative.hpp"
#include "SPatchGraphicsComponents.hpp"

#include <reshade.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>

namespace spatch::graphics::component {
namespace {

enum class State {
    Detached,
    Attaching,
    Attached,
    Failed,
};

std::atomic<State> g_state = State::Detached;

bool IsKnownProfile(const native::ExecutableProfile& profile) noexcept {
    for (const native::ExecutableProfile& known : native::kExecutableProfiles) {
        if (&profile == &known) {
            return true;
        }
    }
    return false;
}

void DetachAttachedPrefix(std::size_t attached) noexcept {
    if (attached >= 7) {
        ao::Detach();
    }
    if (attached >= 6) {
        shadow_scale::Detach();
    }
    if (attached >= 5) {
        tonemapping::Detach();
    }
    if (attached >= 4) {
        sss::Detach(false);
    }
    if (attached >= 3) {
        gi::Detach(false);
    }
    if (attached >= 2) {
        pbr::Detach();
    }
    if (attached >= 1) {
        water::Detach();
    }
}

}  // namespace

bool AttachVerified(
    HMODULE module,
    const native::ExecutableProfile& profile) noexcept {
    State expected = State::Detached;
    if (module == nullptr || !IsKnownProfile(profile) ||
        !g_state.compare_exchange_strong(
            expected, State::Attaching, std::memory_order_acq_rel)) {
        reshade::log::message(
            reshade::log::level::error,
            "[ShenLong] graphics component registration rejected: invalid "
            "module/profile or duplicate attach.");
        return false;
    }

    std::size_t attached = 0;
    try {
        water::Attach(module);
        ++attached;
        pbr::Attach(module);
        ++attached;
        gi::Attach(module);
        ++attached;
        sss::Attach(module);
        ++attached;
        tonemapping::Attach(module);
        ++attached;
        shadow_scale::Attach(module);
        ++attached;
        ao::Attach(module);
        ++attached;
    } catch (...) {
        // ReShade records each callback with separate vector insertions. If an
        // allocation throws during the current component's Attach call, that
        // component may already own a partial callback set even though it was
        // not counted as complete. Its Detach routine is idempotent for
        // missing callbacks, so include the in-progress component in rollback.
        DetachAttachedPrefix(attached + 1);
        g_state.store(State::Failed, std::memory_order_release);
        reshade::log::message(
            reshade::log::level::error,
            "[ShenLong] verified graphics component registration threw; "
            "registered/in-progress prefix detached and module left inert.");
        return false;
    }

    g_state.store(State::Attached, std::memory_order_release);
    char message[256]{};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "[ShenLong] graphics components active for exact profile=%s.",
        profile.id);
    reshade::log::message(reshade::log::level::info, message);
    return true;
}

void Detach(bool process_terminating) noexcept {
    const State previous =
        g_state.exchange(State::Detached, std::memory_order_acq_rel);
    if (previous != State::Attached || process_terminating) {
        return;
    }
    DetachAttachedPrefix(7);
}

}  // namespace spatch::graphics::component
