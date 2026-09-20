#include "plan.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/emit/reserved_words.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// How many bytes a builtin type takes in a constant block, and how each target places it.
struct block_layout
{
    i32 size = 0;
    /// HLSL starts a matrix on a fresh 16-byte row, and anything else wherever it still fits into the current one.
    bool hlsl_takes_new_row = false;
    i32 wgsl_alignment = 0;
    /// MSL aligns a three-vector like WGSL and, unlike it, also sizes it 16: nothing fits into its tail.
    i32 msl_size = 0;
    i32 msl_alignment = 0;
};

/// A size of 0 means the type has no place in a block or on an edge.
block_layout layout_of(builtin b)
{
    switch (b)
    {
    case builtin::scalar_float:
        return {.size = 4, .wgsl_alignment = 4, .msl_size = 4, .msl_alignment = 4};
    case builtin::float3:
    case builtin::vec3:
    case builtin::pos3:
        return {.size = 12, .wgsl_alignment = 16, .msl_size = 16, .msl_alignment = 16};
    case builtin::float4:
    case builtin::hpos4:
        return {.size = 16, .wgsl_alignment = 16, .msl_size = 16, .msl_alignment = 16};
    case builtin::mat4:
        return {.size = 64, .hlsl_takes_new_row = true, .wgsl_alignment = 16, .msl_size = 64, .msl_alignment = 16};
    default:
        return {};
    }
}

i32 round_up(i32 value, i32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/// How many arguments a builtin function takes; -1 for what is no builtin function.
i32 arity_of(builtin b)
{
    switch (b)
    {
    case builtin::normalize:
    case builtin::saturate:
        return 1;
    case builtin::dot:
    case builtin::transform_position:
    case builtin::transform_direction:
    case builtin::scale_color:
    case builtin::multiply:
    case builtin::add:
        return 2;
    default:
        return -1;
    }
}

cc::string_view role_name(struct_role role)
{
    switch (role)
    {
    case struct_role::plain:
        return "struct";
    case struct_role::vertex_input:
        return "vertex input";
    case struct_role::stage_link:
        return "stage-to-stage struct";
    case struct_role::render_targets:
        return "render target struct";
    }
    return "";
}

struct_role input_role(flat_entry_point const& e)
{
    return e.entry_stage == stage::vertex ? struct_role::vertex_input : struct_role::stage_link;
}

struct_role result_role(flat_entry_point const& e)
{
    return e.entry_stage == stage::vertex ? struct_role::stage_link : struct_role::render_targets;
}

struct validator
{
    checked_module const& m;
    flat_entry_point const& e;
    cc::vector<error>& errors;

    void report(error_kind kind, symbol_id symbol, cc::string detail)
    {
        errors.push_back({.kind = kind, .symbol = symbol, .detail = cc::move(detail)});
    }

    void entry_point_name()
    {
        auto where = cc::string();
        for (auto const t : all_targets())
            if (is_reserved(t, e.name))
                where.appendf("{}{}", where.empty() ? "" : ", ", to_string(t));
        if (!where.empty())
            report(error_kind::reserved_entry_point_name, e.function,
                   cc::format("'{}' is reserved in {}", e.name, where));
    }

    void edge_struct(type_id type, struct_role role)
    {
        auto const& info = m.at(type);
        auto const name = m.name_of(type);
        auto positions = 0;
        for (auto const& member : m.at(info.members))
        {
            if (!is_builtin_type(m, member.type) || builtin_of_type(m, member.type) == builtin::mat4)
                report(error_kind::unsupported, info.symbol,
                       cc::format("a member of type '{}' in a {}: '{}.{}'", m.name_of(member.type), role_name(role),
                                  name, member.name));

            if (member.is_position)
            {
                ++positions;
                if (role != struct_role::stage_link)
                    report(error_kind::unsupported, info.symbol,
                           cc::format("@position in a {}: '{}.{}'", role_name(role), name, member.name));
                else if (positions == 2)
                    report(error_kind::unsupported, info.symbol,
                           cc::format("a second @position: '{}.{}'", name, member.name));
            }
            else if (role == struct_role::vertex_input)
            {
                auto const n = cc::string_view(member.name);
                auto const is_sv
                    = n.size() >= 3 && (n[0] == 's' || n[0] == 'S') && (n[1] == 'v' || n[1] == 'V') && n[2] == '_';
                if (is_sv)
                    report(error_kind::system_value_semantic, info.symbol, cc::format("'{}.{}'", name, member.name));
            }
        }
    }

    void bindings()
    {
        auto inline_count = 0;
        for (auto const id : e.bindings)
        {
            auto const& s = m.at(id);
            auto const& b = m.bindings[s.info];
            if (!b.is_inline)
            {
                report(error_kind::unsupported, id, cc::format("a binding that is not @inline: '{}'", s.name));
                continue;
            }
            if (++inline_count == 2)
                report(error_kind::unsupported, id, cc::format("a second @inline binding: '{}'", s.name));

            auto is_placed = true;
            for (auto const& member : m.at(b.members))
                if (!is_builtin_type(m, member.type))
                {
                    is_placed = false;
                    report(error_kind::unsupported, id,
                           cc::format("a member of type '{}' in an @inline binding: '{}.{}'", m.name_of(member.type),
                                      s.name, member.name));
                }
            if (!is_placed)
                continue;

            auto hlsl = 0;
            auto wgsl = 0;
            auto msl = 0;
            for (auto const& member : m.at(b.members))
            {
                auto const l = layout_of(builtin_of_type(m, member.type));
                if (l.hlsl_takes_new_row || hlsl % 16 + l.size > 16)
                    hlsl = round_up(hlsl, 16);
                wgsl = round_up(wgsl, l.wgsl_alignment);
                msl = round_up(msl, l.msl_alignment);
                if (hlsl != wgsl || hlsl != msl)
                {
                    report(error_kind::layout_mismatch, id,
                           cc::format("'{}.{}' is at byte {} in HLSL, at byte {} in WGSL and at byte {} in MSL", s.name,
                                      member.name, hlsl, wgsl, msl));
                    break;
                }
                hlsl += l.size;
                wgsl += l.size;
                msl += l.msl_size;
            }
        }
    }

    void tree()
    {
        for (auto const& x : e.exprs)
        {
            if (x.node.is<flat_invalid>())
                report(error_kind::malformed_tree, e.function, "an unfilled expression");
            else if (auto const* b = x.node.try_as<flat_binding_member>())
            {
                auto is_listed = false;
                for (auto const id : e.bindings)
                    is_listed = is_listed || id == b->binding;
                if (!is_listed)
                    report(error_kind::malformed_tree, e.function,
                           cc::format("a member of '{}', which the entry point does not list", m.at(b->binding).name));
            }
            else if (auto const* l = x.node.try_as<flat_literal>())
            {
                // Only a finite value minus itself is zero.
                if (!(l->value - l->value == 0))
                    report(error_kind::non_finite_literal, e.function,
                           cc::format("a literal of type '{}'", m.name_of(x.type)));
            }
            else if (auto const* c = x.node.try_as<flat_call>())
            {
                if (arity_of(c->intrinsic) != i32(e.at(c->arguments).size()))
                    report(
                        error_kind::malformed_tree, e.function,
                        cc::format("a call of '{}' with {} arguments", m.at(c->callee).name, e.at(c->arguments).size()));
            }
            else if (x.node.is<flat_construct>() && m.at(x.type).is_opaque)
                report(error_kind::malformed_tree, e.function,
                       cc::format("a construction of the opaque '{}'", m.name_of(x.type)));
        }
    }
};

struct planner
{
    plan& p;

    /// A name of the program as this target may spell it: itself, or with a trailing underscore where it is reserved.
    cc::string spell(cc::string_view name)
    {
        if (!is_reserved(p.which, name))
            return name;
        return p.names.mint(cc::format("{}_", name));
    }

    /// A member lives in the scope of its struct, so it only has to differ from its siblings.
    cc::string spell_member(cc::string_view name, cc::span<member_info const> siblings)
    {
        auto result = cc::string(name);
        auto const is_free = [&]
        {
            if (is_reserved(p.which, result))
                return false;
            if (result == name)
                return true;
            for (auto const& s : siblings)
                if (s.name == result)
                    return false;
            return true;
        };
        while (!is_free())
            result += "_";
        return result;
    }

    cc::vector<planned_member> members_of(ast::range_of<member_info> range, bool has_locations)
    {
        auto result = cc::vector<planned_member>();
        auto next_location = 0;
        auto const members = p.m.at(range);
        for (auto const& member : members)
            result.push_back({
                .name = spell_member(member.name, members),
                .source_name = member.name,
                .type = member.type,
                .is_position = has_locations && member.is_position,
                .location = has_locations && !member.is_position ? next_location++ : -1,
            });
        return result;
    }

    /// Post-order, so a struct stands after every struct it holds, which HLSL needs and WGSL does not mind.
    void need(type_id type, struct_role role)
    {
        if (!is_valid(type) || is_builtin_type(p.m, type) || p.m.at(type).kind != type_kind::structure)
            return;
        if (p.struct_of_type[index_of(type)] != -1)
            return;
        // Claimed before the members are visited; the check pass reports a struct that holds itself, so this is no loop.
        p.struct_of_type[index_of(type)] = -2;

        auto const& info = p.m.at(type);
        for (auto const& member : p.m.at(info.members))
            need(member.type, struct_role::plain);

        p.struct_of_type[index_of(type)] = i32(p.structs.size());
        p.structs.push_back({
            .type = type,
            .name = spell(p.m.at(info.symbol).name),
            .role = role,
            .members = members_of(info.members, role != struct_role::plain),
        });
    }

    void constants()
    {
        for (auto const id : p.e.bindings)
        {
            auto const& s = p.m.at(id);
            auto const& b = p.m.bindings[s.info];
            auto planned = planned_constants{
                .symbol = id,
                .name = spell(s.name),
                .block_name = p.names.mint(cc::format("{}_data", s.name)),
                .members = members_of(b.members, false),
            };
            auto offset = 0;
            for (auto& member : planned.members)
            {
                auto const l = layout_of(builtin_of_type(p.m, member.type));
                if (l.hlsl_takes_new_row || offset % 16 + l.size > 16)
                    offset = round_up(offset, 16);
                member.offset = offset;
                offset += l.size;
            }
            p.constants = cc::move(planned);
        }
    }
};
} // namespace

bool sgl::emit::impl::is_builtin_type(check::checked_module const& m, check::type_id type)
{
    return builtin_of_type(m, type) != check::builtin::none;
}

check::builtin sgl::emit::impl::builtin_of_type(check::checked_module const& m, check::type_id type)
{
    auto const& t = m.at(type);
    return t.kind == check::type_kind::structure ? m.at(t.symbol).intrinsic : check::builtin::none;
}

void sgl::emit::impl::validate(check::checked_module const& m, check::flat_entry_point const& e, cc::vector<error>& errors)
{
    auto v = validator{.m = m, .e = e, .errors = errors};
    v.entry_point_name();
    if (e.input == e.result)
        v.report(error_kind::unsupported, e.function,
                 cc::format("one struct as both the parameter and the result: '{}'", m.name_of(e.input)));
    v.edge_struct(e.input, input_role(e));
    if (e.input != e.result)
        v.edge_struct(e.result, result_role(e));
    v.bindings();
    v.tree();
}

sgl::emit::impl::plan sgl::emit::impl::make_plan(check::checked_module const& m, check::flat_entry_point const& e, target t)
{
    auto result = plan{.m = m, .e = e, .which = t, .names = e.names};
    // Reserved first, so nothing minted below can be one of them.
    for (auto const word : reserved_words(t))
        result.names.taken.push_back(word);
    result.struct_of_type.resize_to_filled(m.types.size(), -1);

    auto p = planner{.p = result};
    p.need(e.input, input_role(e));
    p.need(e.result, result_role(e));
    for (auto const& local : e.locals)
        p.need(local.type, struct_role::plain);
    for (auto const& x : e.exprs)
        p.need(x.type, struct_role::plain);
    p.constants();
    for (auto const& local : e.locals)
        result.locals.push_back(p.spell(local.name));
    return result;
}
