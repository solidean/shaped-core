#include "material_type.hh"

#include <clean-core/container/byte_stream_builder.hh>
#include <shaped-viewer/impl/content_hash.hh>

namespace sv
{
material_type material_type::create(cc::string name, cc::vector<material_attribute_decl> signature, cc::string shader)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_string(name);
    b.add_string(shader);
    b.add_pod(i64(signature.size()));
    for (auto const& d : signature)
    {
        b.add_string(d.name);
        b.add_pod(d.format);
        b.add_bool(d.is_final);
        b.add_pod_span_sized(cc::span<byte const>(d.default_value));
    }

    auto type = material_type{.name = cc::move(name),
                              .signature = cc::move(signature),
                              .shader = cc::move(shader),
                              .hash = cc::hash128::create(b.written_bytes(), impl::material_type_hash_seed)};

    for (auto i = 0; i < type.signature.size(); ++i)
        for (auto j = i + 1; j < type.signature.size(); ++j)
            CC_ASSERT(type.signature[i].name != type.signature[j].name, "a material type declares each attribute once");

    for (auto const& d : type.signature)
        CC_ASSERT(d.default_value.size() == d.format.size_bytes(), "a declaration's default must be exactly its "
                                                                   "format's size");

    return type;
}

material_attribute_decl const* material_type::find(cc::string_view name) const
{
    for (auto const& d : signature)
        if (d.name == name)
            return &d;
    return nullptr;
}
} // namespace sv
