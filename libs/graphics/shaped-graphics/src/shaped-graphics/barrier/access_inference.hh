#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics/barrier/footprint.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/resource/views.hh>

/// Shared inference policy: which access / layout an operation or a bound view implies.
/// Backends call these so the semantics of "declare access is never public — infer it from the op" stay consistent across backends.
/// Each backend keeps full freedom over how it tracks and emits barriers.

namespace sg
{
/// The access a shader performs on a bound view of this class — the inferred replacement for an explicit per-binding declaration.
/// Constants blocks read, readonly buffers and textures read, readwrite buffers and images write.
/// An image counts as written whatever its binding's `access` says; libs/graphics/shaped-graphics/docs/TODO.md tracks narrowing that.
[[nodiscard]] constexpr access_flags shader_access_of(view_class c)
{
    switch (c)
    {
    case view_class::constants:
        return access_flag::constants_read;
    case view_class::readonly:
    case view_class::texture:
        return access_flag::shader_read;
    case view_class::readwrite:
    case view_class::image:
        return access_flag::shader_write;
    case view_class::acceleration_structure:
        return access_flag::accel_read;
    }
    return access_flag::shader_read; // unreachable for the closed set above
}
} // namespace sg

/// What a bound view is declared with at an op: its access and the stages it happens in.
struct sg::impl::view_access
{
    access_flags access;
    pipeline_stage_flags stages;
};

namespace sg
{
/// How an op whose pipeline has `footprint` touches the view bound at slot `binding` of group `group` as `bound_as`.
///
/// Nothing where the code never touches the slot, which is what makes an untouched binding cost no barrier.
/// A pipeline with no known footprint falls back to the view's class, at every stage of the op.
[[nodiscard]] inline cc::optional<impl::view_access> access_at(impl::pipeline_footprint const* footprint,
                                                               int group,
                                                               isize binding,
                                                               view_class bound_as,
                                                               pipeline_stage_flags op_stages)
{
    if (footprint == nullptr || !footprint->is_known())
        return impl::view_access{.access = shader_access_of(bound_as), .stages = op_stages};
    auto const use = footprint->use_of(group, binding);
    if (!use.is_touched())
        return {};
    auto const stages = use.stages & op_stages;
    return impl::view_access{.access = use.access, .stages = stages.is_empty() ? op_stages : stages};
}

/// The layout a bound texture view of this class needs (the single inference point for the texture bind
/// path): a texture view wants `shader_texture`, an image view wants `shader_image`.
/// Only texture views reach this — buffers have no layout, and neither does an acceleration structure.
[[nodiscard]] constexpr texture_layout shader_layout_of(view_class c)
{
    switch (c)
    {
    case view_class::constants:
    case view_class::readonly:
    case view_class::readwrite:
    case view_class::acceleration_structure: // never a texture view — buffers/AS never call this
    case view_class::texture:
        return texture_layout::shader_texture;
    case view_class::image:
        return texture_layout::shader_image;
    }
    return texture_layout::shader_texture; // unreachable for the closed set above
}
} // namespace sg
