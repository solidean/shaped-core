#include <shaped-graphics-language/emit/impl/plan.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

bool is_hlsl(emit::target t)
{
    return t == emit::target::hlsl_dx12 || t == emit::target::hlsl_vulkan;
}

/// The array of 3-vectors, at any depth, HLSL holds in GPU memory as one of 4-vectors (EMIT-169).
bool is_3_vector_array(checked_module const& m, type_id type)
{
    if (!is_valid(type) || m.builtin_type_of(type) != nullptr || m.at(type).kind != type_kind::array)
        return false;
    auto const* const record = m.builtin_type_of(innermost_of(m, type));
    return record != nullptr && record->leaf_count == 3;
}

/// The array of mat3, at any depth, HLSL holds in GPU memory as one of `float4x3` (EMIT-168).
bool is_mat3_array(checked_module const& m, type_id type)
{
    return is_valid(type) && m.builtin_type_of(type) == nullptr && m.at(type).kind == type_kind::array
        && is_mat3(m, innermost_of(m, type));
}

/// The planned struct of `type` where it stands in natural GPU memory, as its target declares it there; null elsewhere.
planned_struct const* struct_in_memory(plan const& p, type_id type)
{
    if (!is_valid(type) || p.m.builtin_type_of(type) != nullptr || p.m.at(type).kind != type_kind::structure)
        return nullptr;
    auto const at = p.struct_of_type[index_of(type)];
    if (at < 0 || p.structs[at].memory_size <= 0 || p.structs[at].is_packed)
        return nullptr;
    return &p.structs[at];
}

/// How the member at `index` of `members`, placed in natural memory, is held by the target.
/// `in_constants`: the members stand in a constant block, where HLSL holds a `mat2` as a `float4`.
memory_access access_of(plan const& p, cc::span<planned_member const> members, isize index, bool in_constants)
{
    constexpr cc::string_view lanes[] = {"", ".x", ".xy", ".xyz"};
    auto const& member = members[index];
    // EMIT-172: every target holds an array of one value per row as 4-vectors of its scalar
    if (member.is_row_per_element)
        return {.held = held_as::widened_array,
                .lanes = lanes[p.m.builtin_type_of(p.m.at(member.type).element)->leaf_count]};
    if (is_hlsl(p.which) && is_mat3(p.m, member.type))
        return {.held = held_as::widened_mat3};
    if (is_hlsl(p.which) && in_constants && is_mat2(p.m, member.type))
        return {.held = held_as::vector_mat2};
    if (is_hlsl(p.which) && is_3_vector_array(p.m, member.type))
        return {.held = held_as::widened_array, .lanes = lanes[3]};
    if (is_hlsl(p.which) && is_mat3_array(p.m, member.type))
        return {.held = held_as::widened_array, .lanes = mat3_cell_field(p, member.type)};
    if (p.which == emit::target::msl && shares_tail(p, members, index))
        return {.held = held_as::packed_tail};
    return {};
}

/// Marks `id` raw, and the objects a write through it reaches down to the value it is a part of.
void mark_place(plan& p, flat_expr_id id)
{
    while (is_valid(id))
    {
        p.accesses[index_of(id)].is_raw = true;
        auto const& x = p.e.at(id);
        if (auto const* const member = x.node.try_as<flat_member>())
            id = member->object;
        else if (auto const* const element = x.node.try_as<flat_element>())
            id = element->object;
        else
            break;
    }
}
} // namespace

cc::string_view sgl::emit::impl::mat3_cell_field(plan const& p, check::type_id type)
{
    if (!is_hlsl(p.which) || !is_mat3_array(p.m, type))
        return {};
    auto const element = p.m.at(type).element;
    return p.m.builtin_type_of(element) == nullptr && p.m.at(element).kind == type_kind::array ? ".m" : "";
}

bool sgl::emit::impl::holds_widened_mat3(plan const& p, check::type_id object_type, isize member)
{
    auto const* const s = struct_in_memory(p, object_type);
    return s != nullptr && is_hlsl(p.which) && is_mat3(p.m, p.m.at(p.m.at(object_type).members)[member].type);
}

bool sgl::emit::impl::holds_widened_array(plan const& p, check::type_id object_type, isize member)
{
    auto const* const s = struct_in_memory(p, object_type);
    auto const type = s != nullptr ? p.m.at(p.m.at(object_type).members)[member].type : check::type_id::none;
    return s != nullptr && is_hlsl(p.which) && (is_3_vector_array(p.m, type) || is_mat3_array(p.m, type));
}

bool sgl::emit::impl::is_piece(plan const& p, flat_element const& element)
{
    auto const type = p.e.at(element.object).type;
    return p.accesses[index_of(element.object)].held == held_as::pieces && p.m.builtin_type_of(type) == nullptr
        && p.m.at(type).kind == type_kind::array && p.e.at(element.index).node.is<flat_int_literal>()
        && !is_word_split(p, p.m.at(type).element);
}

bool sgl::emit::impl::is_word_split(plan const& p, check::type_id type)
{
    if (p.which != emit::target::hlsl_dx12 && p.which != emit::target::msl)
        return false;
    auto const* const record = is_valid(type) ? p.m.builtin_type_of(type) : nullptr;
    return record != nullptr && check::is_64_bit(record->leaf_kind) && record->leaf_count <= 4;
}

i32 sgl::emit::impl::word_pieces(plan const& p, check::type_id type)
{
    return is_wide_64_bit(p, type) ? 2 : 1;
}

bool sgl::emit::impl::is_wide_64_bit(plan const& p, check::type_id type)
{
    auto const* const record = is_valid(type) ? p.m.builtin_type_of(type) : nullptr;
    return record != nullptr && check::is_64_bit(record->leaf_kind) && record->leaf_count >= 3 && record->leaf_count <= 4;
}

void sgl::emit::impl::plan_memory_accesses(plan& p)
{
    p.accesses = cc::vector<memory_access>::create_defaulted(p.e.exprs.size());
    for (auto i = isize(0); i < p.e.exprs.size(); ++i)
    {
        auto const& x = p.e.exprs[i];
        if (auto const* const b = x.node.try_as<flat_binding_member>())
        {
            if (resource_of(p, b->binding, b->member) >= 0 || workgroup_of(p, b->binding, b->member) >= 0)
                continue;
            // a memory form addresses its fields itself (EMIT-113)
            auto const& block = *block_of(p, b->binding);
            if (!block.form.has_value())
                p.accesses[i] = access_of(p, block.members, block.block_member_of[b->member], true);
        }
        else if (auto const* const member = x.node.try_as<flat_member>(); member != nullptr && !member->is_swizzle())
        {
            if (auto const* const s = struct_in_memory(p, p.e.at(member->object).type))
                p.accesses[i] = access_of(p, s->members, s->member_of[member->member], s->is_constants);
            // EMIT-174: an edge struct holds an array or a matrix as one member per element or column
            else if (auto const object_type = p.e.at(member->object).type;
                     is_valid(object_type) && p.m.builtin_type_of(object_type) == nullptr
                     && p.m.at(object_type).kind == type_kind::structure && p.struct_of_type[index_of(object_type)] >= 0)
            {
                auto const& planned = p.structs[p.struct_of_type[index_of(object_type)]];
                if (auto const at = planned.member_of[member->member]; at >= 0 && planned.members[at].pieces > 0)
                    p.accesses[i] = {.held = held_as::pieces};
            }
        }
    }

    // A place is written as the target holds it, and so is the object a write through a component or a swizzle reaches.
    for (auto const& s : p.e.stmts)
        if (auto const* const assign = s.node.try_as<flat_assign>())
            mark_place(p, assign->place);
    // An array held as 4-vectors is indexed in place, its element read through its lanes.
    // An array held as pieces is too where the index is a literal, which names the piece (EMIT-174).
    // An element that is an array itself is held as its outer one is, so a pass reaches one level deeper; the tree
    // holds no deeper nest than its arrays' dimensions, so as many passes reach every level.
    for (auto changed = true; changed;)
    {
        changed = false;
        for (auto const& x : p.e.exprs)
            if (auto const* const element = x.node.try_as<flat_element>(); element != nullptr)
            {
                auto const at = &x - p.e.exprs.data();
                auto const object = p.accesses[index_of(element->object)];
                if (object.held == held_as::widened_array || (object.held == held_as::pieces && is_piece(p, *element)))
                    p.accesses[index_of(element->object)].is_raw = true;
                auto const held = object.held != held_as::widened_array ? p.accesses[at].held
                                : is_mat3(p.m, x.type)
                                    ? held_as::widened_mat3 // read narrowed and written widened, as one mat3 is
                                : p.m.builtin_type_of(x.type) == nullptr && p.m.at(x.type).kind == type_kind::array
                                    ? held_as::widened_array
                                    : p.accesses[at].held;
                if (held != p.accesses[at].held)
                {
                    p.accesses[at].held = held;
                    p.accesses[at].lanes = object.lanes;
                    changed = true;
                }
            }
    }
}
