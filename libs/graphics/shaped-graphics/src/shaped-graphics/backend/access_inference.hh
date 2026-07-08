#pragma once

#include <shaped-graphics/backend/resource_access.hh>
#include <shaped-graphics/views.hh>

/// Shared inference policy: which access / layout an operation or a bound view implies. Backends call
/// these so the *semantics* of "declare access is never public — infer it from the op" stay consistent
/// across backends, while each backend keeps full freedom over how it tracks and emits barriers.

namespace sg
{
/// The access a shader performs on a bound view of this class (the inferred replacement for an explicit
/// per-binding declaration). Uniform blocks read; readonly storage reads; readwrite storage writes.
[[nodiscard]] constexpr access_flags shader_access_of(view_class c)
{
    switch (c)
    {
    case view_class::uniform:
        return access_flags::uniform_read;
    case view_class::readonly:
        return access_flags::shader_read;
    case view_class::readwrite:
        return access_flags::shader_write;
    }
    return access_flags::shader_read; // unreachable for the closed set above
}

/// The layout a bound texture view of this class needs (the single inference point for the texture bind
/// path): a sampled/read view wants `shader_read`, a read-write storage view wants `storage`. Unused until
/// the texture bind path lands — buffers have no layout and never call this.
[[nodiscard]] constexpr texture_layout shader_layout_of(view_class c)
{
    switch (c)
    {
    case view_class::uniform:
    case view_class::readonly:
        return texture_layout::shader_read;
    case view_class::readwrite:
        return texture_layout::storage;
    }
    return texture_layout::shader_read; // unreachable for the closed set above
}
} // namespace sg
