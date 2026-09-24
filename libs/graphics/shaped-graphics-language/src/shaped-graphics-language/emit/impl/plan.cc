#include "plan.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/emit/reserved_words.hh>
#include <shaped-graphics-language/legalize/core.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// Where a member lands in each target's constant block; a size of 0 means the type has no place in one.
struct member_layout
{
    builtins::block_layout hlsl;
    builtins::block_layout wgsl;
    builtins::block_layout msl;
};

member_layout layout_of(checked_module const& m, type_id type)
{
    auto const* const record = m.builtin_type_of(type);
    if (record == nullptr)
        return {};
    return {.hlsl = record->hlsl_layout, .wgsl = record->wgsl_layout, .msl = record->msl_layout};
}

i32 round_up(i32 value, i32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/// HLSL packs by rows of 16: a value starts a fresh row when it is aligned to one, or when it does not fit the rest of this one.
i32 hlsl_offset(i32 at, builtins::block_layout l)
{
    return l.alignment >= 16 || at % 16 + l.size > 16 ? round_up(at, 16) : at;
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

/// True for a function name some builtin is written as in this target: a local of that name would hide it.
/// Read from the registry, so a new builtin needs no entry in a target's list of reserved words.
bool is_called_by_a_builtin(checked_module const& m, emit::target t, cc::string_view name)
{
    if (m.builtins == nullptr)
        return false;
    for (auto const& f : m.builtins->functions)
        if (f.write.kind == builtins::spelling_kind::call && f.called_in(language_of(t)) == name)
            return true;
    return false;
}

struct_role input_role(flat_entry_point const& e)
{
    // A compute parameter crosses no edge: it is a system value, or a struct of them.
    if (e.entry_stage == stage::compute)
        return struct_role::plain;
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

    void edge_struct(type_id type, struct_role role) { validate_edge_struct(m, type, role, errors); }

    void bindings()
    {
        auto inline_count = 0;
        auto listed = 0;
        for (auto const id : e.bindings)
        {
            ++listed;
            auto const& s = m.at(id);
            if (!m.bindings[s.info].is_inline)
                continue;
            // Listed and skipped when numbering, so it has to stand last or a group would move under the host.
            if (listed != e.bindings.size())
                report(error_kind::unsupported, id,
                       cc::format("an @inline binding that is not the last of the list: '{}'", s.name));
            if (++inline_count == 2)
                report(error_kind::unsupported, id, cc::format("a second @inline binding: '{}'", s.name));
        }
        for (auto const id : e.bindings)
            validate_binding(m, id, errors);
    }

    void tree()
    {
        if (auto const violation = find_core_violation(e); violation.has_value())
            report(error_kind::not_core, e.function, violation.value().reason);
        for (auto const& s : e.stmts)
            if (s.node.is<flat_print>())
            {
                report(error_kind::unsupported, e.function, "a print, which no target writes yet");
                break;
            }

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
                auto const* const record = m.builtin_function(c->intrinsic);
                if (record == nullptr || record->parameters.size() != e.at(c->arguments).size())
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
        if (!is_reserved(p.which, name) && !is_called_by_a_builtin(p.m, p.which, name))
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
        return members_of(p.m.at(range), has_locations);
    }

    cc::vector<planned_member> members_of(cc::span<member_info const> members, bool has_locations)
    {
        auto result = cc::vector<planned_member>();
        auto next_location = 0;
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

    /// Every case of it, in declaration order, since a reader wants the set and not the subset an arm named.
    void need_enum(type_id type)
    {
        if (!is_valid(type) || p.m.at(type).kind != type_kind::enumeration)
            return;
        if (p.enum_of_type[index_of(type)] != -1)
            return;

        auto const& info = p.m.at(type);
        auto const name = p.m.at(info.symbol).name;
        auto planned = planned_enum{.type = type};
        for (auto const& c : p.m.at(info.cases))
            planned.case_names.push_back(spell(cc::format("{}_{}", name, c.name)));

        p.enum_of_type[index_of(type)] = i32(p.enums.size());
        p.enums.push_back(cc::move(planned));
    }

    void resources()
    {
        auto group = 0;
        for (auto const id : p.e.bindings)
        {
            auto const& s = p.m.at(id);
            auto const& b = p.m.bindings[s.info];
            if (b.is_inline)
                continue; // sg addresses the inline constants itself, so they take no group of their own
            auto slot = first_resource_slot(p.m, b);
            auto const members = p.m.at(b.members);
            for (auto i = isize(0); i < members.size(); ++i)
            {
                auto const& t = p.m.at(members[i].type);
                if (!check::is_resource(t.kind))
                    continue;
                p.resources.push_back({.binding = id,
                                       .member = i32(i),
                                       .name = p.names.mint(cc::format("{}_{}", s.name, members[i].name)),
                                       .host_name = cc::format("{}.{}", s.name, members[i].name),
                                       .type = members[i].type,
                                       .element = t.element,
                                       .is_mut = t.is_mut,
                                       .static_sampler = members[i].static_sampler,
                                       .group = group,
                                       .slot = slot++,
                                       .group_name = cc::format("{}_bindings", s.name)});
            }
            ++group;
        }
    }

    /// A group's plain members are a constant block the group owns, as its first resource.
    void group_blocks()
    {
        auto group = 0;
        for (auto const id : p.e.bindings)
        {
            auto const& s = p.m.at(id);
            auto const& b = p.m.bindings[s.info];
            if (b.is_inline)
                continue;
            auto const plain = plain_members_of(p.m, b);
            if (!plain.empty())
            {
                // The binding is a module-level declaration, so its name is its own unless the target reserves it.
                auto planned = planned_constants{
                    .symbol = id,
                    .name = spell(s.name),
                    .host_name = s.name,
                    .block_name = p.names.mint(cc::format("{}_data", s.name)),
                    .members = members_of(plain, false),
                    .group = group,
                    .slot = 0,
                    .group_name = cc::format("{}_bindings", s.name),
                };
                auto const placed = place_block(p.m, plain);
                for (auto i = isize(0); i < planned.members.size(); ++i)
                    planned.members[i].offset = placed.offsets[i];
                auto next = 0;
                for (auto const& member : p.m.at(b.members))
                    planned.block_member_of.push_back(check::is_resource(p.m.at(member.type).kind) ? -1 : next++);
                p.group_blocks.push_back(cc::move(planned));
            }
            ++group;
        }
    }

    void constants()
    {
        for (auto const id : p.e.bindings)
        {
            auto const& s = p.m.at(id);
            auto const& b = p.m.bindings[s.info];
            if (!b.is_inline)
                continue;
            auto planned = planned_constants{
                .symbol = id,
                .name = spell(s.name),
                .host_name = s.name,
                .block_name = p.names.mint(cc::format("{}_data", s.name)),
                .members = members_of(b.members, false),
            };
            auto const placed = place_block(p.m, p.m.at(b.members));
            for (auto i = isize(0); i < planned.members.size(); ++i)
            {
                planned.members[i].offset = placed.offsets[i];
                planned.block_member_of.push_back(i32(i));
            }
            p.constants = cc::move(planned);
        }
    }
};
} // namespace

sgl::builtins::language sgl::emit::impl::language_of(target t)
{
    switch (t)
    {
    case target::hlsl_dx12:
    case target::hlsl_vulkan:
        return builtins::language::hlsl;
    case target::wgsl:
        return builtins::language::wgsl;
    case target::msl:
        return builtins::language::msl;
    }
    return builtins::language::hlsl;
}

sgl::i32 sgl::emit::impl::resource_of(plan const& p, check::symbol_id binding, i32 member)
{
    for (auto i = isize(0); i < p.resources.size(); ++i)
        if (p.resources[i].binding == binding && p.resources[i].member == member)
            return i32(i);
    return -1;
}

cc::string_view sgl::emit::impl::builtin_spelling(plan const& p, cc::string_view name)
{
    auto const type = p.m.builtins->find_type(name);
    return is_valid(type) ? p.m.builtins->at(type).spelled_in(language_of(p.which)) : cc::string_view();
}

bool sgl::emit::impl::is_builtin_type(check::checked_module const& m, check::type_id type)
{
    return m.builtin_type_of(type) != nullptr;
}

cc::string sgl::emit::impl::stream_of(check::member_info const& member)
{
    if (!member.stream.empty())
        return member.stream;
    return cc::string(member.is_per_instance ? "per_instance" : "per_vertex");
}

void sgl::emit::impl::validate_edge_struct(check::checked_module const& m,
                                           check::type_id type,
                                           struct_role role,
                                           cc::vector<error>& errors)
{
    auto const report = [&](error_kind kind, symbol_id symbol, cc::string detail)
    { errors.push_back({.kind = kind, .symbol = symbol, .detail = cc::move(detail)}); };

    auto const& info = m.at(type);
    auto const name = m.name_of(type);
    auto positions = 0;
    for (auto const& member : m.at(info.members))
    {
        if ((member.is_per_instance || !member.stream.empty()) && role != struct_role::vertex_input)
            report(error_kind::unsupported, info.symbol,
                   cc::format("@per_instance or @stream in a {}: '{}.{}'", role_name(role), name, member.name));
        if (role == struct_role::vertex_input)
            for (auto const& other : m.at(info.members))
                if (&other < &member && stream_of(other) == stream_of(member)
                    && other.is_per_instance != member.is_per_instance)
                {
                    report(error_kind::unsupported, info.symbol,
                           cc::format("stream '{}' of '{}' mixes per-vertex and per-instance members",
                                      stream_of(member), name));
                    break;
                }

        auto const* const record = m.builtin_type_of(member.type);
        if (record == nullptr || !record->crosses_edges)
            report(error_kind::unsupported, info.symbol,
                   cc::format("a member of type '{}' in a {}: '{}.{}'", m.name_of(member.type), role_name(role), name,
                              member.name));

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

void sgl::emit::impl::validate_binding(check::checked_module const& m, check::symbol_id id, cc::vector<error>& errors)
{
    auto const report = [&](error_kind kind, cc::string detail)
    { errors.push_back({.kind = kind, .symbol = id, .detail = cc::move(detail)}); };

    auto const& s = m.at(id);
    auto const& b = m.bindings[s.info];

    // A plain member is a constant of a block: the `@inline` one, or the constant buffer its group owns.
    // A resource is a slot of its own in a group, and has no place in an `@inline` block.
    auto is_placed = true;
    for (auto const& member : m.at(b.members))
    {
        if (!b.is_inline && check::is_resource(m.at(member.type).kind))
            continue;
        if (layout_of(m, member.type).hlsl.size != 0)
            continue;
        is_placed = false;
        report(error_kind::unsupported,
               cc::format("a member of type '{}' in {}: '{}.{}'", m.name_of(member.type),
                          b.is_inline ? "an @inline binding" : "a binding", s.name, member.name));
    }
    if (!is_placed)
        return;

    auto hlsl = 0;
    auto wgsl = 0;
    auto msl = 0;
    for (auto const& member : plain_members_of(m, b))
    {
        auto const l = layout_of(m, member.type);
        hlsl = hlsl_offset(hlsl, l.hlsl);
        wgsl = round_up(wgsl, l.wgsl.alignment);
        msl = round_up(msl, l.msl.alignment);
        if (hlsl != wgsl || hlsl != msl)
        {
            report(error_kind::layout_mismatch, cc::format("'{}.{}' is at byte {} in HLSL, at byte {} in WGSL and at "
                                                           "byte {} in MSL",
                                                           s.name, member.name, hlsl, wgsl, msl));
            return;
        }
        hlsl += l.hlsl.size;
        wgsl += l.wgsl.size;
        msl += l.msl.size;
    }
}

cc::vector<sgl::check::member_info> sgl::emit::impl::plain_members_of(check::checked_module const& m,
                                                                      check::binding_info const& b)
{
    auto result = cc::vector<check::member_info>();
    for (auto const& member : m.at(b.members))
        if (!check::is_resource(m.at(member.type).kind))
            result.push_back(member);
    return result;
}

sgl::i32 sgl::emit::impl::first_resource_slot(check::checked_module const& m, check::binding_info const& b)
{
    return !b.is_inline && !plain_members_of(m, b).empty() ? 1 : 0;
}

sgl::emit::impl::planned_constants const* sgl::emit::impl::block_of(plan const& p, check::symbol_id binding)
{
    if (p.constants.has_value() && p.constants.value().symbol == binding)
        return &p.constants.value();
    for (auto const& block : p.group_blocks)
        if (block.symbol == binding)
            return &block;
    return nullptr;
}

sgl::emit::impl::block_placement sgl::emit::impl::place_block(check::checked_module const& m,
                                                              cc::span<check::member_info const> members)
{
    auto result = block_placement();
    auto offset = 0;
    for (auto const& member : members)
    {
        auto const l = layout_of(m, member.type).hlsl;
        offset = hlsl_offset(offset, l);
        result.offsets.push_back(offset);
        result.sizes.push_back(l.size);
        offset += l.size;
    }
    result.size = offset;
    return result;
}

void sgl::emit::impl::validate(check::checked_module const& m, check::flat_entry_point const& e, cc::vector<error>& errors)
{
    auto v = validator{.m = m, .e = e, .errors = errors};
    if (e.input == e.result && e.entry_stage != stage::compute)
        v.report(error_kind::unsupported, e.function,
                 cc::format("one struct as both the parameter and the result: '{}'", m.name_of(e.input)));
    // The struct spelling of the thread id needs the signedness question settled first (the spec's bindings file).
    if (e.entry_stage == stage::compute && !e.takes_thread_id)
        v.report(error_kind::unsupported, e.function,
                 "a @compute fun whose parameter is a struct; write `@thread_id id: int3` for now");

    // A compute entry point has no pipeline edge at either end, so neither struct is judged as one.
    if (e.entry_stage != stage::compute)
    {
        v.edge_struct(e.input, input_role(e));
        if (e.input != e.result)
            v.edge_struct(e.result, result_role(e));
    }
    v.bindings();
    v.tree();
}

sgl::emit::impl::plan sgl::emit::impl::make_plan(check::checked_module const& m, check::flat_entry_point const& e, target t)
{
    auto result = plan{.m = m, .e = e, .which = t, .names = e.names};
    // Reserved first, so nothing minted below can be one of them.
    for (auto const word : reserved_words(t))
        result.names.taken.push_back(word);
    if (m.builtins != nullptr)
        for (auto const& f : m.builtins->functions)
            if (f.write.kind == builtins::spelling_kind::call)
                result.names.taken.push_back(f.called_in(language_of(t)));
    result.struct_of_type.resize_to_filled(m.types.size(), -1);
    result.enum_of_type.resize_to_filled(m.types.size(), -1);

    auto p = planner{.p = result};
    p.need(e.input, input_role(e));
    p.need(e.result, result_role(e));
    for (auto const& local : e.locals)
    {
        p.need(local.type, struct_role::plain);
        p.need_enum(local.type);
    }
    for (auto const& x : e.exprs)
    {
        p.need(x.type, struct_role::plain);
        p.need_enum(x.type);
    }
    // `spell` is what mints `<name>_` where the target reserves the name or a builtin is called by it.
    result.entry_name = p.spell(e.name);
    p.constants();
    p.group_blocks();
    p.resources();
    // The check pass minted the locals, so a buffer or a block minted above never took one's name.
    for (auto const& local : e.locals)
        result.locals.push_back(p.spell(local.name));
    if (e.entry_stage == stage::compute && !result.locals.empty())
        result.dispatch_name = result.names.mint(cc::format("{}_in", result.locals[0]));
    return result;
}
