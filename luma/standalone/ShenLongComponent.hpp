#pragma once

#include <Windows.h>

namespace spatch::graphics::native {
struct ExecutableProfile;
}

namespace spatch::graphics::component {

// Registers the ReShade-facing graphics components only for the exact profile
// supplied by ShenLong's synchronous pre-device identity bootstrap.
bool AttachVerified(
    HMODULE module,
    const native::ExecutableProfile& profile) noexcept;

void Detach(bool process_terminating) noexcept;

}  // namespace spatch::graphics::component
