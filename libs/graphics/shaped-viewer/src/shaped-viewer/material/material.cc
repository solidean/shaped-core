#include "material.hh"

#include <clean-core/container/byte_stream_builder.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/material/impl/material_hash.hh>

namespace sv
{
material material::create(cc::string name, material_type_id type, cc::vector<material_attribute_binding> overrides)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(type);
    b.add_pod(i64(overrides.size()));
    for (auto const& o : overrides)
    {
        b.add_string(o.name);
        b.add_pod(o.kind);
        b.add_bool(o.is_final);
        b.add_pod_span_sized(cc::span<byte const>(o.constant));
        b.add_pod(o.sample.texture);
        b.add_string(o.sample.uv_attribute);
        impl::add_sampler(b, o.sample.sampler);
    }

    return {.name = cc::move(name),
            .type = type,
            .overrides = cc::move(overrides),
            .hash = cc::hash128::create(b.written_bytes(), impl::material_definition_hash_seed)};
}

material_attribute_binding const* material::find(cc::string_view name) const
{
    for (auto const& o : overrides)
        if (o.name == name)
            return &o;
    return nullptr;
}
} // namespace sv
