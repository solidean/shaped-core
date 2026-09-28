#include "structural_hash.hh"

#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::check;

namespace
{
void fold_type(cc::byte_stream_builder& b, checked_module const& m, type_id type);

// By value: which slot of `checked_module::samplers` a sampler landed in is no part of its shape.
void fold_sampler(cc::byte_stream_builder& b, sampler_state const& s)
{
    b.add_pod(s.min_filter);
    b.add_pod(s.mag_filter);
    b.add_pod(s.mip_filter);
    b.add_pod(s.address_u);
    b.add_pod(s.address_v);
    b.add_pod(s.address_w);
    b.add_pod(s.compare);
    b.add_pod(s.max_anisotropy);
    b.add_pod(s.min_lod);
    b.add_pod(s.max_lod);
    b.add_pod(s.mip_lod_bias);
}

void fold_members(cc::byte_stream_builder& b, checked_module const& m, cc::span<member_info const> members)
{
    b.add_pod(u64(members.size()));
    for (auto const& member : members)
    {
        b.add_string(member.name);
        b.add_bool(member.is_position);
        b.add_bool(member.is_thread_id);
        b.add_bool(member.is_per_instance);
        b.add_string(member.stream);
        b.add_bool(member.is_unfilterable);
        b.add_bool(member.is_non_filtering);
        b.add_bool(member.static_sampler >= 0);
        if (member.static_sampler >= 0)
            fold_sampler(b, m.samplers[member.static_sampler]);
        fold_type(b, m, member.type);
    }
}

void fold_type(cc::byte_stream_builder& b, checked_module const& m, type_id type)
{
    auto const& t = m.at(type);
    b.add_pod(u8(t.kind));
    switch (t.kind)
    {
    case type_kind::buffer:
        b.add_bool(t.is_mut);
        fold_type(b, m, t.element);
        return;
    case type_kind::enumeration:
        b.add_string(m.name_of(type));
        for (auto const& c : m.at(t.cases))
        {
            b.add_string(c.name);
            b.add_pod(c.value);
        }
        return;
    case type_kind::structure:
        // A builtin is its name; a struct of the program is its members, whatever it is called.
        if (m.builtin_type_of(type) != nullptr || t.is_opaque)
            b.add_string(m.name_of(type));
        else
            fold_members(b, m, m.at(t.members));
        return;
    case type_kind::texture:
    case type_kind::image:
    case type_kind::sampler:
        b.add_pod(u8(t.shape));
        b.add_bool(t.is_depth);
        b.add_pod(t.format);
        b.add_pod(u8(t.access));
        b.add_bool(t.is_comparison);
        b.add_bool(t.element != type_id::none);
        if (t.element != type_id::none)
            fold_type(b, m, t.element);
        return;
    default:
        return;
    }
}
} // namespace

cc::hash128 check::structural_hash(checked_module const& m, type_id type)
{
    auto b = cc::byte_stream_builder();
    fold_type(b, m, type);
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 check::structural_hash(checked_module const& m, cc::span<member_info const> members)
{
    auto b = cc::byte_stream_builder();
    fold_members(b, m, members);
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::string check::hex_of(cc::hash128 hash)
{
    return cc::format("{:016x}{:016x}", hash.high, hash.low);
}
