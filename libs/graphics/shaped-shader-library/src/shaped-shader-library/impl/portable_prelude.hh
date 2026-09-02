#pragma once

#include <clean-core/container/span.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// The portable-HLSL prelude, baked in from shaders/sc/portable.hlsli at configure time.
/// One entry, "portable.hlsli", which every shader_library mounts at `sc`.
/// Static storage, so the embedded_filesystem built over it may outlive any particular library.
[[nodiscard]] cc::span<embedded_file const> portable_prelude_files();
} // namespace slib::impl
