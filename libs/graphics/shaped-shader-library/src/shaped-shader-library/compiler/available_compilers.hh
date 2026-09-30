#pragma once

#include <shaped-shader-library/fwd.hh>

namespace slib
{
/// Registers every compiler edge this build can make with `lib`: HLSL, WGSL and metal where they exist, and SGL over each.
///
/// What a library serving any backend wants, since a `shader_asset` picks its edge by what the acquiring context accepts.
/// Registering an edge compiles nothing, so an edge no context ever asks for costs nothing.
/// A compiler whose creation fails, such as DXC without its DLLs, is left out with a warning naming why; an acquire through it then fails as unreachable.
void add_available_compilers(shader_library& lib);
} // namespace slib
