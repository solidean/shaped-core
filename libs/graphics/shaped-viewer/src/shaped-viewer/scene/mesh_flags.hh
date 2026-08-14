#pragma once

#include <clean-core/common/flags.hh>
#include <shaped-viewer/fwd.hh>

/// What a mesh is allowed to be, as a set of independent bits.
///
/// TODO: provisional, and nothing consumes these yet.
/// The set will grow as the renderer learns to honor it — pickability, backface culling, wireframe overlay, ...
enum class sv::mesh_flag
{
    visible,         // drawn at all; a hidden mesh still occupies its resources
    casts_shadow,    // contributes occlusion to shadow rays
    receives_shadow, // is shadowed by others, rather than lit as if unoccluded
};

CC_FLAG_ENUM_INDEXED(sv, mesh_flag, u32);

namespace sv
{
/// A SET of mesh_flag — what a mesh stores, never the bare enum.
using mesh_flags = cc::flags<mesh_flag>;

/// What a mesh gets unless it says otherwise: visible and fully participating in shadowing.
/// The empty set is a mesh that draws nothing, so a default-constructed `mesh_flags` is deliberately NOT the default here.
inline constexpr mesh_flags mesh_flags_default
    = mesh_flag::visible | mesh_flag::casts_shadow | mesh_flag::receives_shadow;
} // namespace sv
