// Unified ShenLong graphics ASI for Sleeping Dogs: Definitive Edition.
//
// ReShade invokes callbacks in registration order. Keeping every lighting
// component in one module makes the water, PBR, GI, SSS, and SDAO/coordinator
// order deterministic. Texture filtering registers first so sampler policy is
// active before any component can create device resources. AgX is autonomous:
// it replaces the exact final pre-HUD pixel shader during pipeline creation
// and does not participate in draw callbacks. Shadow scale is autonomous from
// ReShade draw events: it rewrites the shadow shaders' baked 2048-atlas filter
// constants during pipeline creation and uses native D3D11 context detours to
// resize shadow maps and correct their viewports.

#include <Windows.h>
#include <reshade.hpp>

#include "ShenLongComponent.hpp"
#include "ShenLongNative.hpp"

namespace {

constexpr wchar_t kTargetExecutable[] = L"sdhdship.exe";
bool g_addon_registered = false;

bool IsTargetProcess() noexcept {
    wchar_t executable_path[MAX_PATH]{};
    const DWORD path_length = GetModuleFileNameW(
        nullptr, executable_path, static_cast<DWORD>(ARRAYSIZE(executable_path)));
    if (path_length == 0 || path_length >= ARRAYSIZE(executable_path)) {
        return false;
    }

    const wchar_t* executable_name = executable_path;
    for (const wchar_t* cursor = executable_path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            executable_name = cursor + 1;
        }
    }
    return CompareStringOrdinal(
               executable_name, -1, kTargetExecutable, -1, TRUE) == CSTR_EQUAL;
}

bool AttachVerifiedComponents(
    HMODULE module,
    const spatch::graphics::native::ExecutableProfile& profile) noexcept {
    return spatch::graphics::component::AttachVerified(module, profile);
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "ShenLong";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Verified renderer options for Sleeping Dogs: Definitive Edition";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!IsTargetProcess()) {
            return TRUE;
        }
        if (!reshade::register_addon(module)) {
            return FALSE;
        }
        g_addon_registered = true;
        // ReShade event registration above stays inside the earliest attach
        // window. Graphics components, fixed-RVA hooks, and settings mutation
        // remain inert until the one-shot pre-device callback verifies the
        // exact executable SHA-256 and PE identity after loader lock has been
        // released and before the real D3D11CreateDevice call.
        if (!spatch::graphics::native::Attach(
                module, &AttachVerifiedComponents)) {
            reshade::unregister_addon(module);
            g_addon_registered = false;
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        spatch::graphics::native::Detach(reserved != nullptr);
        if (reserved != nullptr) {
            // During process termination, Windows has already stopped other
            // threads. They may have owned ReShade, D3D, or component locks;
            // touching those subsystems under loader lock can deadlock. The OS
            // is reclaiming all process resources, so no explicit teardown is
            // required.
            return TRUE;
        }
        if (g_addon_registered) {
            spatch::graphics::component::Detach(false);
            reshade::unregister_addon(module);
            g_addon_registered = false;
        }
    }
    return TRUE;
}
