#pragma once

#include <clean-core/container/byte_stream_builder.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_texture.hh>

namespace sv::impl
{
/// Appends `s` to `b` field by field.
///
/// Field by field rather than `add_pod(s)` because a `sg::sampler` has padding, and padding bytes are indeterminate — two samplers
/// that compare equal would then hash differently depending on how each was built.
/// A material key that is unstable across equal values would mint a second permutation for a material nothing changed about.
inline void add_sampler(cc::byte_stream_builder& b, sg::sampler const& s)
{
    b.add_pod(s.min_filter);
    b.add_pod(s.mag_filter);
    b.add_pod(s.mip_filter);
    b.add_pod(s.address_u);
    b.add_pod(s.address_v);
    b.add_pod(s.address_w);
    b.add_pod(s.mip_lod_bias);
    b.add_pod(s.max_anisotropy);
    b.add_pod(s.min_lod);
    b.add_pod(s.max_lod);
    b.add_optional(s.compare);
    b.add_pod(s.border_color);
}

/// Appends the `component_count` selectors of `z` that are actually read.
///
/// Only those, which is the canonicalization the plan calls for: two swizzles that differ solely in a selector no attribute of
/// this format reads are one permutation, and hash alike.
inline void add_swizzle(cc::byte_stream_builder& b, channel_swizzle const& z, int component_count)
{
    for (auto i = 0; i < component_count; ++i)
        b.add_pod(z.components[i]);
}
} // namespace sv::impl
