#pragma once

#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/resource/views.hh>

/// Shared inference policy: which access / layout an operation or a bound view implies.
/// Backends call these so the semantics of "declare access is never public — infer it from the op" stay consistent across backends.
/// Each backend keeps full freedom over how it tracks and emits barriers.

namespace sg
{
/// The access a shader performs on a bound view of this class — the inferred replacement for an explicit per-binding declaration.
/// Uniform blocks read, readonly storage reads, readwrite storage writes.
[[nodiscard]] constexpr access_flags shader_access_of(view_class c)
{
    switch (c)
    {
    case view_class::uniform:
        return access_flag::uniform_read;
    case view_class::readonly:
        return access_flag::shader_read;
    case view_class::readwrite:
        return access_flag::shader_write;
    case view_class::acceleration_structure:
        return access_flag::accel_read;
    }
    return access_flag::shader_read; // unreachable for the closed set above
}

/// The layout a bound texture view of this class needs (the single inference point for the texture bind
/// path): a sampled/read view wants `shader_readonly`, a read-write storage view wants `shader_readwrite`.
/// Only textures reach this — buffers have no layout, and neither does an acceleration structure.
[[nodiscard]] constexpr texture_layout shader_layout_of(view_class c)
{
    switch (c)
    {
    case view_class::uniform:
    case view_class::readonly:
    case view_class::acceleration_structure: // never a texture — buffers/AS never call this
        return texture_layout::shader_readonly;
    case view_class::readwrite:
        return texture_layout::shader_readwrite;
    }
    return texture_layout::shader_readonly; // unreachable for the closed set above
}
} // namespace sg
