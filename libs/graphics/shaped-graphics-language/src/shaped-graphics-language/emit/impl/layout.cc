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

placed_members place_from(checked_module const& m,
                          cc::span<member_info const> members,
                          address_space space,
                          i32 base,
                          cc::span<i32 const> prefix);

/// What one element of an array of `type` takes in `space`: the bytes it strides by and its alignment.
struct element_shape
{
    i32 stride = 0;
    i32 alignment = 1;
};

element_shape element_of(checked_module const& m, type_id type, address_space space)
{
    if (auto const* const record = m.builtin_type_of(type))
    {
        auto const alignment = alignment_in(*record, space);
        return {.stride = round_up(memory_size_of(*record, space), alignment), .alignment = alignment};
    }
    if (m.at(type).kind == type_kind::array)
    {
        auto const inner = element_of(m, m.at(type).element, space);
        return {.stride = i32(m.at(type).count) * inner.stride, .alignment = inner.alignment};
    }
    auto const inner = place_from(m, m.at(m.at(type).members), space, 0, {});
    auto const alignment = space == address_space::constants ? round_up(inner.alignment, 16) : inner.alignment;
    return {.stride = round_up(inner.size, alignment), .alignment = alignment};
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
            auto const size = memory_size_of(*record, space);
            auto const alignment = alignment_in(*record, space);
            at = round_up(at, alignment);
            result.alignment = alignment > result.alignment ? alignment : result.alignment;
            result.offsets.push_back(at);
            result.sizes.push_back(size);
            result.leaves.push_back({.path = cc::move(path), .type = type, .offset = base + at, .size = size});
            at += size;
            continue;
        }

        // An array strides by its element rounded up to the element's alignment, and starts a row in a constant block.
        // C++ aligns it to its scalar, which EMIT-154 refuses in a packed block wherever that is no row.
        if (m.at(type).kind == type_kind::array)
        {
            auto const& array = m.at(type);
            // EMIT-172: each element of a `@padding(.every_element)` array starts a row of its own
            auto const element
                = members[i].padding == check::padding_kind::every_element && space == address_space::constants
                    ? element_shape{.stride = 16, .alignment = 16}
                    : element_of(m, array.element, space);
            auto const alignment
                = space == address_space::constants ? round_up(element.alignment, 16) : element.alignment;
            at = round_up(at, alignment);
            result.alignment = alignment > result.alignment ? alignment : result.alignment;
            auto const size = i32(array.count) * element.stride;
            result.offsets.push_back(at);
            result.sizes.push_back(size);
            result.leaves.push_back({.path = cc::move(path),
                                     .type = array.element,
                                     .offset = base + at,
                                     .size = size,
                                     .count = i32(array.count)});
            at += size;
            continue;
        }

        // A nested struct starts at its own alignment, which a dry run over its members gives.
        // In a constant block that is at least a row, and what follows it starts the next row.
        auto alignment = place_from(m, m.at(m.at(type).members), space, 0, path).alignment;
        if (space == address_space::constants)
            alignment = round_up(alignment, 16);
        at = round_up(at, alignment);
        auto inner = place_from(m, m.at(m.at(type).members), space, base + at, path);
        result.alignment = alignment > result.alignment ? alignment : result.alignment;
        result.offsets.push_back(at);
        result.sizes.push_back(inner.size);
        for (auto& leaf : inner.leaves)
            result.leaves.push_back(cc::move(leaf));
        at += round_up(inner.size, alignment);
    }
    result.end = at;
    result.size = round_up(at, result.alignment);
    return result;
}
} // namespace

sgl::i32 sgl::emit::impl::scalar_size_of(builtins::type_record const& record)
{
    return check::scalar_bytes_of(record.leaf_kind);
}

sgl::i32 sgl::emit::impl::natural_alignment_of(builtins::type_record const& record)
{
    // WGSL's own alignment is this rule's, for every builtin that has one there
    return record.wgsl_layout.alignment;
}

sgl::i32 sgl::emit::impl::alignment_in(builtins::type_record const& record, address_space space)
{
    if (is_packed(space))
        return scalar_size_of(record);
    if (space == address_space::constants && record.hlsl == "float2x2")
        return 16;
    return natural_alignment_of(record);
}

sgl::i32 sgl::emit::impl::memory_size_of(builtins::type_record const& record, address_space space)
{
    return is_packed(space) ? record.leaf_count * scalar_size_of(record) : record.wgsl_layout.size;
}

bool sgl::emit::impl::is_mat3(check::checked_module const& m, check::type_id type)
{
    auto const* const record = m.builtin_type_of(type);
    return record != nullptr && record->leaf_count == 9;
}

sgl::check::type_id sgl::emit::impl::innermost_of(check::checked_module const& m, check::type_id type)
{
    while (is_valid(type) && m.builtin_type_of(type) == nullptr && m.at(type).kind == type_kind::array)
        type = m.at(type).element;
    return type;
}

bool sgl::emit::impl::is_mat2(check::checked_module const& m, check::type_id type)
{
    auto const* const record = m.builtin_type_of(type);
    return record != nullptr && record->hlsl == "float2x2";
}

bool sgl::emit::impl::is_storage(address_space space)
{
    return space == address_space::storage || space == address_space::packed_storage;
}

bool sgl::emit::impl::is_packed(address_space space)
{
    return space == address_space::packed_constants || space == address_space::packed_storage;
}

cc::string_view sgl::emit::impl::space_name(address_space space)
{
    switch (space)
    {
    case address_space::constants:
        return "constant block";
    case address_space::storage:
        return "storage buffer";
    case address_space::packed_constants:
        return "packed constant block";
    case address_space::packed_storage:
        return "packed storage buffer";
    }
    return {};
}

sgl::emit::impl::address_space sgl::emit::impl::block_space(check::binding_info const& b)
{
    return b.padding == check::padding_kind::none ? address_space::packed_constants : address_space::constants;
}

sgl::emit::impl::address_space sgl::emit::impl::element_space(check::binding_info const& b,
                                                              check::member_info const& member)
{
    return b.padding == check::padding_kind::none || member.padding == check::padding_kind::none
             ? address_space::packed_storage
             : address_space::storage;
}

bool sgl::emit::impl::is_placeable(check::checked_module const& m, check::type_id type)
{
    return first_unplaceable(m, type).empty();
}

cc::string sgl::emit::impl::first_unplaceable(check::checked_module const& m, check::type_id type)
{
    if (auto const* const record = m.builtin_type_of(type))
        return record->hlsl_layout.size != 0 ? cc::string() : cc::format(": {}", m.name_of(type));
    // an array of placeable elements is placeable, at any count but none
    if (is_valid(type) && m.at(type).kind == type_kind::array && m.builtin_type_of(type) == nullptr)
    {
        if (m.at(type).count == 0)
            return cc::format(": {}", m.name_of(type));
        auto const inner = first_unplaceable(m, m.at(type).element);
        return inner.empty() || !inner.starts_with(":") ? inner : cc::format(": {}", m.name_of(type));
    }
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

sgl::i32 sgl::emit::impl::element_stride(check::checked_module const& m, check::type_id element, address_space space)
{
    if (auto const* const record = m.builtin_type_of(element))
        return round_up(memory_size_of(*record, space), alignment_in(*record, space));
    if (m.at(element).kind == check::type_kind::atomic)
        return element_stride(m, m.at(element).element, space);
    return place_struct(m, element, space).size;
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
    // an array places its element struct as a member places it
    while (is_valid(type) && m.builtin_type_of(type) == nullptr && m.at(type).kind == type_kind::array)
        type = m.at(type).element;
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
        if (is_storage(space) && t.kind == check::type_kind::buffer && element_space(b, member) == space)
            collect_structs(m, t.element, out);
        else if (space == block_space(b) && !check::is_resource(t.kind))
            collect_structs(m, member.type, out);
    }
}
