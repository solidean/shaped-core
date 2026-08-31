#include <clean-core/container/byte_stream_builder.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>
#include <shaped-graphics/binding/sampler.hh>

namespace sg::impl
{
namespace
{
/// Folds a sampler in field by field.
/// Never add_pod over the whole struct: its padding bytes would make two logically-equal samplers hash differently.
void add_sampler(cc::byte_stream_builder& b, sampler const& s)
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

void add_binding(cc::byte_stream_builder& b, binding const& bnd)
{
    b.add_optional(bnd.group_index);
    b.add_optional(bnd.space);
    b.add_pod(bnd.index);
    b.add_pod(bnd.count);
    b.add_pod(bnd.type);
    b.add_optional(bnd.block_size);
    b.add_optional(bnd.texture_dimension);
}
} // namespace

cc::hash128 binding_group_layout_hash(cc::span<binding const> bindings, cc::span<named_sampler const> static_samplers)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(u64(bindings.size()));
    for (auto const& bnd : bindings)
    {
        b.add_string(bnd.name); // the name is part of the identity here: a static sampler is matched to a binding by it
        add_binding(b, bnd);
    }
    b.add_pod(u64(static_samplers.size()));
    for (auto const& ns : static_samplers)
    {
        b.add_string(ns.name);
        add_sampler(b, ns.sampler);
    }
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 sampler_hash(sampler const& s)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    add_sampler(b, s);
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_layout_hash(cc::span<binding_group_layout_handle const> groups,
                                 cc::span<bound_sampler const> static_samplers,
                                 cc::optional<binding> const& inline_constants)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(u64(groups.size()));
    for (auto const& g : groups)
    {
        // A null group is a hole in the slot order, and two layouts that differ only in where their holes are are
        // different layouts — so it contributes a distinct value rather than being skipped.
        auto const group_hash = g != nullptr ? g->structural_hash() : cc::hash128{};
        b.add_pod(group_hash);
    }

    // Pipeline-level static samplers change the root signature, so they are part of the identity.
    b.add_pod(u64(static_samplers.size()));
    for (auto const& bs : static_samplers)
    {
        add_binding(b, bs.binding);
        add_sampler(b, bs.sampler);
    }

    // Inline constants add a 32-bit-constants root parameter, so they are part of the identity too.
    b.add_pod(inline_constants.has_value());
    if (inline_constants.has_value())
        add_binding(b, inline_constants.value());

    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_layout_hash(pipeline_layout_description const& desc)
{
    return pipeline_layout_hash(desc.groups, desc.static_samplers, desc.inline_constants);
}
} // namespace sg::impl
