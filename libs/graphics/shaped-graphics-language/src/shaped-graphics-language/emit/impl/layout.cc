#include "layout.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit::impl;

i32 round_up(i32 value, i32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

bool is_struct(checked_module const& m, type_id type)
{
    return is_valid(type) && m.builtin_type_of(type) == nullptr && m.at(type).kind == type_kind::structure
        && !m.at(type).is_opaque;
}

/// Places `members` from `base`, appending every leaf with its offset from the outermost value.
/// `prefix` is the path down to these members.
placed_members place_from(checked_module const& m,
                          cc::span<member_info const> members,
                          address_space space,
                          i32 base,
                          cc::span<i32 const> prefix)
{
    auto result = placed_members();
    auto at = 0;
    for (auto i = isize(0); i < members.size(); ++i)
    {
        auto const type = members[i].type;
        auto path = cc::vector<i32>();
        for (auto const step : prefix)
            path.push_back(step);
        path.push_back(i32(i));
        if (type == checked_module::void_type)
        {
            result.offsets.push_back(0);
            result.sizes.push_back(0);
            continue;
        }

        if (auto const* const record = m.builtin_type_of(type))
        {
            auto const l = record->hlsl_layout;
            // HLSL packs by rows of 16: a value starts a fresh row when it is aligned to one, or when it would cross one.
            if (space == address_space::constants)
                at = l.alignment >= 16 || at % 16 + l.size > 16 ? round_up(at, 16) : at;
            result.offsets.push_back(at);
            result.sizes.push_back(l.size);
            result.leaves.push_back({.path = cc::move(path), .type = type, .offset = base + at, .size = l.size});
            at += l.size;
            continue;
        }

        // A nested struct starts a fresh row in a constant block, and what follows it packs against its last member.
        if (space == address_space::constants)
            at = round_up(at, 16);
        auto inner = place_from(m, m.at(m.at(type).members), space, base + at, path);
        result.offsets.push_back(at);
        result.sizes.push_back(inner.size);
        for (auto& leaf : inner.leaves)
            result.leaves.push_back(cc::move(leaf));
        at += inner.size;
    }
    result.size = at;
    return result;
}
} // namespace

cc::string_view sgl::emit::impl::space_name(address_space space)
{
    return space == address_space::constants ? "constant block" : "storage buffer";
}

bool sgl::emit::impl::is_placeable(check::checked_module const& m, check::type_id type)
{
    return first_unplaceable(m, type).empty();
}

cc::string sgl::emit::impl::first_unplaceable(check::checked_module const& m, check::type_id type)
{
    if (auto const* const record = m.builtin_type_of(type))
        return record->hlsl_layout.size != 0 ? cc::string() : cc::format(": {}", m.name_of(type));
    // an atomic stands in memory as the integer it updates
    if (is_valid(type) && m.at(type).kind == type_kind::atomic)
        return first_unplaceable(m, m.at(type).element);
    if (!is_struct(m, type))
        return cc::format(": {}", m.name_of(type));
    for (auto const& member : m.at(m.at(type).members))
    {
        if (member.type == checked_module::void_type)
            continue;
        auto const inner = first_unplaceable(m, member.type);
        if (!inner.empty())
            return cc::format(".{}{}", member.name, inner);
    }
    return {};
}

sgl::emit::impl::placed_members sgl::emit::impl::place(check::checked_module const& m,
                                                       cc::span<check::member_info const> members,
                                                       address_space space)
{
    return place_from(m, members, space, 0, {});
}

sgl::emit::impl::placed_members sgl::emit::impl::place_struct(check::checked_module const& m,
                                                              check::type_id type,
                                                              address_space space)
{
    return place_from(m, m.at(m.at(type).members), space, 0, {});
}

sgl::i32 sgl::emit::impl::element_stride(check::checked_module const& m, check::type_id element)
{
    if (auto const* const record = m.builtin_type_of(element))
        return record->hlsl_layout.size;
    if (m.at(element).kind == check::type_kind::atomic)
        return element_stride(m, m.at(element).element);
    return place_struct(m, element, address_space::storage).size;
}

cc::vector<cc::string> sgl::emit::impl::padding_of(check::checked_module const& m,
                                                   cc::span<check::member_info const> members,
                                                   address_space space)
{
    auto const placed = place(m, members, space);
    auto result = cc::vector<cc::string>();
    auto end = 0;
    auto previous = cc::string_view();
    for (auto i = isize(0); i < members.size(); ++i)
    {
        if (members[i].type == checked_module::void_type)
            continue;
        auto const offset = placed.offsets[i];
        if (offset > end)
            result.push_back(previous.empty() ? cc::format("'{}' starts at byte {}", members[i].name, offset)
                                              : cc::format("'{}' starts at byte {}, {} bytes past where '{}' ends",
                                                           members[i].name, offset, offset - end, previous));
        end = offset + placed.sizes[i];
        previous = members[i].name;
    }
    return result;
}

void sgl::emit::impl::collect_structs(check::checked_module const& m, check::type_id type, cc::vector<check::type_id>& out)
{
    if (!is_struct(m, type))
        return;
    for (auto const known : out)
        if (known == type)
            return;
    for (auto const& member : m.at(m.at(type).members))
        collect_structs(m, member.type, out);
    out.push_back(type);
}

void sgl::emit::impl::collect_placed_structs(check::checked_module const& m,
                                             check::symbol_id binding,
                                             address_space space,
                                             cc::vector<check::type_id>& out)
{
    auto const& b = m.bindings[m.at(binding).info];
    // workgroup memory is laid out by each target alone, since no host writes it
    if (b.is_workgroup)
        return;
    for (auto const& member : m.at(b.members))
    {
        // a binding array of buffers holds its element as a lone buffer does
        auto const& whole = m.at(member.type);
        auto const& t = m.takes_slots(member.type) && whole.kind == check::type_kind::array ? m.at(whole.element) : whole;
        if (space == address_space::storage && t.kind == check::type_kind::buffer)
            collect_structs(m, t.element, out);
        else if (space == address_space::constants && !check::is_resource(t.kind))
            collect_structs(m, member.type, out);
    }
}
