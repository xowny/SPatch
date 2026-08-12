#pragma once

#include <Windows.h>
#include <cstdint>

struct ID3D11Buffer;
struct ID3D11ShaderResourceView;

namespace reshade::api {
struct command_list;
struct device;
}  // namespace reshade::api

namespace spatch::graphics::gi {

void Attach(HMODULE module);
void Detach(bool process_terminating) noexcept;

// Consumes the GI work prepared by the first final-composition callback and
// issues the sole native composition draw. The AO coordinator calls this
// from the last callback so SSS can update the HDR lighting buffer in between.
// A null AO override leaves the game's t7 binding untouched; a non-null view
// is bound only for the draw and the previous t7 binding is restored afterward.
bool DrawPreparedComposition(
    reshade::api::command_list* command_list,
    ID3D11ShaderResourceView* ambient_occlusion_override,
    std::uint32_t index_count,
    std::uint32_t instance_count,
    std::uint32_t first_index,
    std::int32_t vertex_offset,
    std::uint32_t first_instance) noexcept;

bool IsEnabled(reshade::api::device* device) noexcept;

}  // namespace spatch::graphics::gi

namespace spatch::graphics::sss {

void Attach(HMODULE module);
void Detach(bool process_terminating) noexcept;

}  // namespace spatch::graphics::sss

namespace spatch::graphics::pbr {

void Attach(HMODULE module);
void Detach() noexcept;

}  // namespace spatch::graphics::pbr

namespace spatch::graphics::water {

void Attach(HMODULE module);
void Detach() noexcept;

}  // namespace spatch::graphics::water

namespace spatch::graphics::tonemapping {

void Attach(HMODULE module);
void Detach() noexcept;

}  // namespace spatch::graphics::tonemapping

namespace spatch::graphics::shadow_scale {

void Attach(HMODULE module);
void Detach() noexcept;

}  // namespace spatch::graphics::shadow_scale

namespace spatch::graphics::ao {

void Attach(HMODULE module);
void Detach() noexcept;

// Borrowed projection constants captured from the game's native AO pass.
// The returned buffer remains owned by the AO coordinator.
ID3D11Buffer* GetNativeAoConstants(
    reshade::api::command_list* command_list) noexcept;

}  // namespace spatch::graphics::ao
