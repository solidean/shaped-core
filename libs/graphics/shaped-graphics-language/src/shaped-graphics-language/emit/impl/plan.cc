#include "plan.hh"

#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/emit/impl/layout.hh>
#include <shaped-graphics-language/emit/reserved_words.hh>
#include <shaped-graphics-language/legalize/core.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

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
    case struct_role::patch_constants:
        return "factors struct";
    }
    return "";
}

/// True for a name some builtin writes in this target: a local of that name would hide it.
/// Read from the registry, so a new builtin needs no entry in a target's list of reserved words.
bool is_called_by_a_builtin(checked_module const& m, emit::target t, cc::string_view name)
{
    if (m.builtins == nullptr)
        return false;
    for (auto const& f : m.builtins->functions)
        if (f.writes_name(language_of(t), name))
            return true;
    return false;
}

struct_role input_role(flat_entry_point const& e)
{
    // A compute parameter crosses no edge: it is a system value, or a struct of them.
    // a ray-tracing stage's payload is a plain struct the target hands over by reference
    if (e.entry_stage == stage::compute || e.entry_stage >= stage::raygen)
        return struct_role::plain;
    // the geometry and the tessellation stages take an array of what the stage before hands on
    return e.entry_stage == stage::vertex ? struct_role::vertex_input : struct_role::stage_link;
}

struct_role result_role(flat_entry_point const& e)
{
    if (e.entry_stage == stage::tessellation_control)
        return struct_role::patch_constants;
    return e.entry_stage == stage::vertex || e.entry_stage == stage::tessellation_evaluation
             ? struct_role::stage_link
             : struct_role::render_targets;
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
        auto groups = 0;
        auto inline_binding = cc::optional<symbol_id>();
        auto reported_order = false;
        for (auto const id : e.bindings)
        {
            auto const& s = m.at(id);
            // workgroup memory is listed like a group and takes none: no host binds it, so it may stand anywhere
            if (m.bindings[s.info].is_workgroup)
                continue;
            if (!m.bindings[s.info].is_inline)
            {
                // The inline binding is skipped when numbering, so a group after it would move under the host.
                if (inline_binding.has_value() && !reported_order)
                {
                    reported_order = true;
                    report(error_kind::unsupported, inline_binding.value(),
                           cc::format("an @inline binding a group of the list follows: '{}'",
                                      m.at(inline_binding.value()).name));
                }
                // Refused on every target, so an entry point written for one is written for all of them.
                if (groups++ == k_max_groups)
                    report(error_kind::too_many_groups, id,
                           cc::format("'{}' is group {}, and sg binds {} besides the inline constants", s.name,
                                      k_max_groups, k_max_groups));
                continue;
            }
            inline_binding = id;
            if (++inline_count == 2)
                report(error_kind::unsupported, id, cc::format("a second @inline binding: '{}'", s.name));
        }
        for (auto const id : e.bindings)
            validate_binding(m, id, errors);
    }

    /// EMIT-133: refused on every target, so an entry point written for one is written for all of them.
    void file_samplers()
    {
        auto reported = cc::vector<symbol_id>();
        for (auto const& x : e.exprs)
        {
            auto const* const smp = x.node.try_as<check::flat_file_sampler>();
            if (smp == nullptr || file_sampler_index(m, smp->sampler) < k_max_file_samplers)
                continue;
            auto is_repeat = false;
            for (auto const r : reported)
                is_repeat = is_repeat || r == smp->sampler;
            if (is_repeat)
                continue;
            reported.push_back(smp->sampler);
            report(
                error_kind::too_many_samplers, smp->sampler,
                cc::format("'{}' reaches sampler '{}' at index {}, and a stage holds {}; every sampler declared above "
                           "it counts toward its index, whether reached or not",
                           e.name, m.at(smp->sampler).name, file_sampler_index(m, smp->sampler), k_max_file_samplers));
        }
    }

    void tree()
    {
        if (auto const violation = find_core_violation(m, e); violation.has_value())
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
                auto const expected = record == nullptr ? -1
                                                        : record->parameters.size() + (record->takes_element ? 1 : 0)
                                                              + (record->takes_acceleration_index ? 1 : 0);
                if (record == nullptr || expected != e.at(c->arguments).size())
                    report(
                        error_kind::malformed_tree, e.function,
                        cc::format("a call of '{}' with {} arguments", m.at(c->callee).name, e.at(c->arguments).size()));
            }
            else if (x.node.is<flat_construct>() && m.at(x.type).is_opaque)
                report(error_kind::malformed_tree, e.function,
                       cc::format("a construction of the opaque '{}'", m.name_of(x.type)));

            // EMIT-107: every field of it is void, and a struct of no member is no struct in WGSL.
            auto const& t = m.at(x.type);
            auto is_all_void = t.kind == type_kind::structure && t.members.count > 0 && !is_builtin_type(m, x.type);
            if (is_all_void)
                for (auto const& member : m.at(t.members))
                    is_all_void = is_all_void && member.type == checked_module::void_type;
            if (is_all_void)
            {
                report(error_kind::unsupported, e.function,
                       cc::format("the struct '{}', whose every field is void", m.name_of(x.type)));
                break;
            }
        }
    }
};

struct planner
{
    plan& p;
    /// Every spelling a struct or an enum case was given.
    cc::set<cc::string> type_spellings;
    /// Where a factors struct's locations start: after the control point's, which SPIR-V counts in the same space.
    i32 patch_location_base = 0;

    /// A name of the program as this target may spell it: itself, or with a trailing underscore where it is reserved.
    cc::string spell(cc::string_view name)
    {
        if (!is_reserved(p.which, name) && !is_called_by_a_builtin(p.m, p.which, name))
            return name;
        return p.names.mint(cc::format("{}_", name));
    }

    /// `spell` for the name of a struct or an enum case, which the text declares at its top level.
    /// A struct of the user file may shadow one of the prelude's, so a second of one name is minted one of its own.
    cc::string spell_type(cc::string_view name)
    {
        auto result = spell(name);
        if (type_spellings.contains(result))
            result = p.names.mint(result);
        type_spellings.insert(result);
        return result;
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

    cc::vector<planned_member> members_of(cc::span<member_info const> members, bool has_locations, i32 first_location = 0)
    {
        auto result = cc::vector<planned_member>();
        auto next_location = first_location;
        for (auto const& member : members)
            result.push_back({
                .name = spell_member(member.name, members),
                .source_name = member.name,
                .type = member.type,
                .is_position = has_locations && member.is_position,
                .interpolate = member.interpolate,
                .output = member.output,
                .factor = member.factor,
                .location = has_locations && !member.is_position && member.output == check::pixel_output::color
                                 && member.factor == check::tessellation_factor::none
                              ? next_location++
                              : -1,
            });
        return result;
    }

    /// An array type's spelling, after its element's, whose role is the array's: a patch's control points link stages.
    void need_array(type_id type, struct_role role)
    {
        if (p.array_of_type[index_of(type)] != -1)
            return;
        auto const& info = p.m.at(type);
        need(info.element, role);
        need_enum(info.element);
        auto const element = element_text(info.element);
        auto const is_hlsl = p.which == emit::target::hlsl_dx12 || p.which == emit::target::hlsl_vulkan;
        p.array_of_type[index_of(type)] = i32(p.array_texts.size());
        p.array_texts.push_back(is_hlsl ? element : cc::format("array<{}, {}>", element, info.count));
    }

    /// What an array's element is spelled as, which `type_text` answers once the plan is done.
    cc::string element_text(type_id type) const
    {
        if (auto const* const record = p.m.builtin_type_of(type))
            return cc::string(record->spelled_in(language_of(p.which)));
        auto const& info = p.m.at(type);
        if (info.kind == type_kind::enumeration)
            return cc::string(builtin_spelling(p, builtins::k_int));
        if (info.kind == type_kind::array)
            return p.array_texts[p.array_of_type[index_of(type)]];
        if (info.kind == type_kind::atomic)
            return cc::string(atomic_text(p, type));
        return p.structs[p.struct_of_type[index_of(type)]].name;
    }

    /// Post-order, so a struct stands after every struct it holds, which HLSL needs and WGSL does not mind.
    void need(type_id type, struct_role role)
    {
        // a binding array is declared with its resource, and has no type of its own to spell
        if (is_valid(type) && !is_builtin_type(p.m, type) && p.m.at(type).kind == type_kind::array)
            return p.m.takes_slots(type) ? void() : need_array(type, role);
        if (!is_valid(type) || is_builtin_type(p.m, type) || p.m.at(type).kind != type_kind::structure)
            return;
        if (p.struct_of_type[index_of(type)] != -1)
            return;
        // Claimed before the members are visited; the check pass reports a struct that holds itself, so this is no loop.
        p.struct_of_type[index_of(type)] = -2;

        auto const& info = p.m.at(type);
        auto written = cc::vector<member_info>();
        auto member_of = cc::vector<i32>();
        for (auto const& member : p.m.at(info.members))
        {
            need(member.type, struct_role::plain);
            member_of.push_back(member.type == check::checked_module::void_type ? -1 : i32(written.size()));
            if (member.type != check::checked_module::void_type)
                written.push_back(member);
        }

        p.struct_of_type[index_of(type)] = i32(p.structs.size());
        p.structs.push_back({
            .type = type,
            .name = spell_type(p.m.at(info.symbol).name),
            .role = role,
            .members = members_of(written, role != struct_role::plain,
                                  role == struct_role::patch_constants ? patch_location_base : 0),
            .member_of = cc::move(member_of),
        });
    }

    /// Every case of it, in declaration order, since a reader wants the set and not the subset an arm named.
    void need_enum(type_id type)
    {
        if (!p.m.is_plain_enum(type))
            return;
        if (p.enum_of_type[index_of(type)] != -1)
            return;

        auto const& info = p.m.at(type);
        auto const name = p.m.at(info.symbol).name;
        auto planned = planned_enum{.type = type};
        for (auto const& c : p.m.at(info.cases))
            planned.case_names.push_back(spell_type(cc::format("{}_{}", name, c.name)));

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
            if (b.is_inline || b.is_workgroup)
                continue; // sg addresses the inline constants itself, so they take no group of their own
            auto slot = first_resource_slot(p.m, b);
            auto const members = p.m.at(b.members);
            for (auto i = isize(0); i < members.size(); ++i)
            {
                // a binding array is its element's resource, at as many consecutive slots as it has elements
                auto const& whole = p.m.at(members[i].type);
                auto const is_array = whole.kind == type_kind::array;
                auto const& t = is_array ? p.m.at(whole.element) : whole;
                if (!check::is_resource(t.kind))
                    continue;
                auto const count = is_array ? whole.count : 1;
                p.resources.push_back({.binding = id,
                                       .member = i32(i),
                                       .name = p.names.mint(cc::format("{}_{}", s.name, members[i].name)),
                                       .host_name = cc::format("{}.{}", s.name, members[i].name),
                                       .type = is_array ? whole.element : members[i].type,
                                       .element = t.element,
                                       .is_mut = t.is_mut,
                                       .group = group,
                                       .slot = slot,
                                       .count = count});
                slot += count;
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
            if (b.is_inline || b.is_workgroup)
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
                };
                auto const placed = place_block(p.m, plain);
                for (auto i = isize(0); i < planned.members.size(); ++i)
                    planned.members[i].offset = placed.offsets[i];
                auto next = 0;
                for (auto const& member : p.m.at(b.members))
                    planned.block_member_of.push_back(p.m.takes_slots(member.type) ? -1 : next++);
                p.group_blocks.push_back(cc::move(planned));
            }
            ++group;
        }
    }

    /// MSL has no global resources, so each group that declares anything is a struct of its slots and a parameter.
    void argument_buffers()
    {
        if (p.which != emit::target::msl)
            return;
        auto group = 0;
        for (auto const id : p.e.bindings)
        {
            auto const& s = p.m.at(id);
            auto const& b = p.m.bindings[s.info];
            if (b.is_inline || b.is_workgroup)
                continue;
            auto is_declared = false;
            for (auto const& block : p.group_blocks)
                is_declared = is_declared || block.group == group;
            for (auto const& r : p.resources)
                is_declared = is_declared || r.group == group;
            if (is_declared)
                p.argument_buffers.push_back({.group = group,
                                              .struct_name = p.names.mint(cc::format("{}_arguments", s.name)),
                                              .parameter = p.names.mint(cc::format("{}_group", s.name))});
            ++group;
        }
    }

    /// Every member of a `@workgroup` binding is a variable of its own, minted `<binding>_<member>`.
    void workgroup_memory()
    {
        for (auto const id : p.e.bindings)
        {
            auto const& s = p.m.at(id);
            auto const& b = p.m.bindings[s.info];
            if (!b.is_workgroup)
                continue;
            auto const members = p.m.at(b.members);
            for (auto i = isize(0); i < members.size(); ++i)
            {
                need(members[i].type, struct_role::plain);
                need_enum(members[i].type);
                p.workgroup.push_back({.binding = id,
                                       .member = i32(i),
                                       .name = p.names.mint(cc::format("{}_{}", s.name, members[i].name)),
                                       .type = members[i].type});
            }
        }
    }

    /// EMIT-133: each file-scope sampler the code reaches, once, at the index its declaration order gives it.
    void file_samplers()
    {
        for (auto const& x : p.e.exprs)
        {
            auto const* const smp = x.node.try_as<check::flat_file_sampler>();
            if (smp == nullptr || sampler_of(p, smp->sampler) >= 0)
                continue;
            auto const& s = p.m.at(smp->sampler);
            auto const index = file_sampler_index(p.m, smp->sampler);
            auto at = isize(0);
            while (at < p.samplers.size() && p.samplers[at].index < index)
                ++at;
            p.samplers.insert_at(
                at, {.symbol = smp->sampler, .name = spell(s.name), .host_name = s.name, .type = s.type, .index = index});
        }
    }

    /// What WGSL and MSL need to reach SGL's layout, where their own rule would not (memory_form.hh).
    void memory_forms()
    {
        auto const form_of_block = [&](planned_constants& block)
        {
            auto const& b = p.m.bindings[p.m.at(block.symbol).info];
            block.form = memory_form_of(p.m, plain_members_of(p.m, b), address_space::constants, 0, p.which);
            if (block.form.has_value())
                block.form.value().name = block.block_name;
        };
        if (p.constants.has_value())
            form_of_block(p.constants.value());
        for (auto& block : p.group_blocks)
            form_of_block(block);
        for (auto& r : p.resources)
        {
            if (p.m.at(r.type).kind != type_kind::buffer)
                continue;
            r.element_form = element_form_of(p.m, r.element, p.which);
            if (r.element_form.has_value())
                r.element_form.value().name = p.names.mint(cc::format("{}_memory", p.m.name_of(r.element)));
        }
    }

    /// Each struct in GPU memory states where its members sit, which `hlsl-vulkan` writes as `[[vk::offset]]`.
    void struct_offsets()
    {
        for (auto const id : p.e.bindings)
        {
            auto const& b = p.m.bindings[p.m.at(id).info];
            // workgroup memory is laid out by each target alone
            if (b.is_workgroup)
                continue;
            for (auto const& member : p.m.at(b.members))
            {
                auto const& whole = p.m.at(member.type);
                auto const& t
                    = p.m.takes_slots(member.type) && whole.kind == type_kind::array ? p.m.at(whole.element) : whole;
                if (t.kind == type_kind::buffer)
                    offsets_of(t.element, address_space::storage);
                else if (!check::is_resource(t.kind))
                    offsets_of(member.type, address_space::constants);
            }
        }
    }

    void offsets_of(type_id type, address_space space)
    {
        auto structs = cc::vector<type_id>();
        collect_structs(p.m, type, structs);
        for (auto const s : structs)
        {
            auto const at = p.struct_of_type[index_of(s)];
            if (at < 0)
                continue;
            auto& planned = p.structs[at];
            auto const placed = place_struct(p.m, s, space);
            for (auto i = isize(0); i < planned.member_of.size(); ++i)
                if (planned.member_of[i] >= 0)
                    planned.members[planned.member_of[i]].offset = placed.offsets[i];
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

sgl::i32 sgl::emit::impl::workgroup_of(plan const& p, check::symbol_id binding, i32 member)
{
    for (auto i = isize(0); i < p.workgroup.size(); ++i)
        if (p.workgroup[i].binding == binding && p.workgroup[i].member == member)
            return i32(i);
    return -1;
}

sgl::i32 sgl::emit::impl::sampler_of(plan const& p, check::symbol_id symbol)
{
    for (auto i = isize(0); i < p.samplers.size(); ++i)
        if (p.samplers[i].symbol == symbol)
            return i32(i);
    return -1;
}

sgl::i32 sgl::emit::impl::file_sampler_index(check::checked_module const& m, check::symbol_id symbol)
{
    auto index = 0;
    for (auto i = isize(0); i < index_of(symbol); ++i)
        index += m.symbols[i].kind == check::symbol_kind::sampler ? 1 : 0;
    return index;
}

sgl::i32 sgl::emit::impl::resource_of(plan const& p, check::symbol_id binding, i32 member)
{
    for (auto i = isize(0); i < p.resources.size(); ++i)
        if (p.resources[i].binding == binding && p.resources[i].member == member)
            return i32(i);
    return -1;
}

cc::string_view sgl::emit::impl::atomic_text(plan const& p, check::type_id type)
{
    auto const is_signed = p.m.name_of(p.m.at(type).element) == builtins::k_int;
    switch (p.which)
    {
    case target::hlsl_dx12:
    case target::hlsl_vulkan:
        return is_signed ? "int" : "uint";
    case target::wgsl:
        return is_signed ? "atomic<i32>" : "atomic<u32>";
    case target::msl:
        return is_signed ? "atomic_int" : "atomic_uint";
    }
    return {};
}

cc::string sgl::emit::impl::array_dimensions(plan const& p, check::type_id type)
{
    if (p.which != target::hlsl_dx12 && p.which != target::hlsl_vulkan)
        return {};
    auto result = cc::string();
    for (; is_valid(type) && p.m.at(type).kind == type_kind::array; type = p.m.at(type).element)
        result.appendf("[{}]", p.m.at(type).count);
    return result;
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

        // a tessellation factor is an array HLSL passes as a system value, which crosses as no member does
        auto const* const record = m.builtin_type_of(member.type);
        auto const is_factor = role == struct_role::patch_constants && member.factor != check::tessellation_factor::none;
        if ((record == nullptr || !record->crosses_edges) && !is_factor)
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

namespace
{
bool contains(cc::span<check::type_id const> types, check::type_id type)
{
    for (auto const t : types)
        if (t == type)
            return true;
    return false;
}
} // namespace

void sgl::emit::impl::validate_binding(check::checked_module const& m, check::symbol_id id, cc::vector<error>& errors)
{
    auto const report = [&](error_kind kind, cc::string detail)
    { errors.push_back({.kind = kind, .symbol = id, .detail = cc::move(detail)}); };
    // What is about a struct rather than this binding, which every binding placing the struct finds the same.
    auto const report_on_struct = [&](error_kind kind, check::type_id type, cc::string detail)
    {
        auto e = error{.kind = kind, .symbol = m.at(type).symbol, .detail = cc::move(detail)};
        for (auto const& known : errors)
            if (known == e)
                return;
        errors.push_back(cc::move(e));
    };

    auto const& s = m.at(id);
    auto const& b = m.bindings[s.info];
    // workgroup memory is laid out by each target alone, since no host writes it
    if (b.is_workgroup)
        return;

    // A plain member is a constant of a block: the `@inline` one, or the constant buffer its group owns.
    // A resource is a slot of its own in a group, and has no place in an `@inline` block; a buffer's element is placed too.
    auto is_placed = true;
    for (auto const& member : m.at(b.members))
    {
        auto const& t = m.at(member.type);
        if (!b.is_inline && m.takes_slots(member.type))
        {
            // a binding array of buffers places its element as a lone buffer does
            auto const& r = t.kind == check::type_kind::array ? m.at(t.element) : t;
            if (r.kind == check::type_kind::buffer && !is_placeable(m, r.element))
            {
                is_placed = false;
                auto const inner = first_unplaceable(m, r.element);
                report(error_kind::unsupported,
                       cc::format("a buffer of '{}' in a binding: '{}.{}'{}", m.name_of(r.element), s.name, member.name,
                                  inner.contains("bool") ? ", whose bool has no layout; bool32 has one" : ""));
            }
            continue;
        }
        if (is_placeable(m, member.type))
            continue;
        is_placed = false;
        auto const inner = first_unplaceable(m, member.type);
        report(error_kind::unsupported,
               cc::format("a member of type '{}' in {}: '{}.{}'{}", m.name_of(member.type),
                          b.is_inline ? "an @inline binding" : "a binding", s.name, member.name,
                          inner.contains("bool") ? ", whose bool has no layout; bool32 has one" : ""));
    }
    if (!is_placed)
        return;

    // A struct has one layout, so it stands in one address space; two rules would give it two.
    for (auto const space : {address_space::constants, address_space::storage})
    {
        auto const other = space == address_space::constants ? address_space::storage : address_space::constants;
        auto mine = cc::vector<check::type_id>();
        collect_placed_structs(m, id, space, mine);
        for (auto const type : mine)
            for (auto const& info : m.bindings)
            {
                auto theirs = cc::vector<check::type_id>();
                collect_placed_structs(m, info.symbol, other, theirs);
                if (!contains(theirs, type))
                    continue;
                // Worded from the constant block's side, so the binding on either side finds the same error.
                auto const is_constants = space == address_space::constants;
                report_on_struct(error_kind::layout_conflict, type,
                                 cc::format("'{}' is in a constant block of '{}' and in a storage buffer of '{}'",
                                            m.name_of(type), is_constants ? s.name : m.at(info.symbol).name,
                                            is_constants ? m.at(info.symbol).name : s.name));
                break;
            }
    }

    auto const plain = plain_members_of(m, b);
    if (b.is_no_padding)
        for (auto& gap : padding_of(m, plain, address_space::constants))
            report(error_kind::padding_forbidden, cc::format("in the block of @no_padding '{}': {}", s.name, gap));
    for (auto const space : {address_space::constants, address_space::storage})
    {
        auto structs = cc::vector<check::type_id>();
        collect_placed_structs(m, id, space, structs);
        for (auto const type : structs)
            if (m.at(type).is_no_padding)
                for (auto& gap : padding_of(m, m.at(m.at(type).members), space))
                    report_on_struct(error_kind::padding_forbidden, type,
                                     cc::format("in @no_padding '{}', as a {} places it: {}", m.name_of(type),
                                                space_name(space), gap));
    }
}

cc::vector<sgl::check::member_info> sgl::emit::impl::plain_members_of(check::checked_module const& m,
                                                                      check::binding_info const& b)
{
    auto result = cc::vector<check::member_info>();
    for (auto const& member : m.at(b.members))
        if (!m.takes_slots(member.type))
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
    auto placed = place(m, members, address_space::constants);
    return {.offsets = cc::move(placed.offsets), .sizes = cc::move(placed.sizes), .size = placed.size};
}

void sgl::emit::impl::validate(check::checked_module const& m, check::flat_entry_point const& e, cc::vector<error>& errors)
{
    auto v = validator{.m = m, .e = e, .errors = errors};
    if (e.input == e.result && e.entry_stage != stage::compute)
        v.report(error_kind::unsupported, e.function,
                 cc::format("one struct as both the parameter and the result: '{}'", m.name_of(e.input)));

    // A compute entry point has no pipeline edge at either end, so neither struct is judged as one, and a ray-tracing
    // stage's payload is handed over by reference rather than across an edge.
    if (e.entry_stage != stage::compute && e.entry_stage < stage::raygen)
    {
        // the geometry and the tessellation stages take an array of what crosses, and a geometry stage hands its
        // vertices on through its stream rather than its result
        auto input = e.input;
        if (check::is_valid(input) && m.at(input).kind == type_kind::array)
            input = m.at(input).element;
        if (check::is_valid(input))
            v.edge_struct(input, input_role(e));
        if (e.entry_stage == stage::geometry)
        {
            for (auto const& local : e.locals)
                if (local.kind == check::local_kind::parameter && m.at(local.type).kind == type_kind::stream)
                    v.edge_struct(m.at(local.type).element, struct_role::stage_link);
        }
        else if (input != e.result)
            v.edge_struct(e.result, result_role(e));
    }
    v.bindings();
    v.tree();
    v.file_samplers();
}

sgl::emit::impl::plan sgl::emit::impl::make_plan(check::checked_module const& m, check::flat_entry_point const& e, target t)
{
    auto result = plan{.m = m, .e = e, .which = t, .names = e.names};
    // Reserved first, so nothing minted below can be one of them.
    for (auto const word : reserved_words(t))
        (void)result.names.reserve(word);
    if (m.builtins != nullptr)
        for (auto const& f : m.builtins->functions)
        {
            if (f.write.kind == builtins::spelling_kind::call)
                (void)result.names.reserve(f.called_in(language_of(t)));
            for (auto const name : f.names_in(language_of(t)))
                (void)result.names.reserve(name);
        }
    result.struct_of_type.resize_to_filled(m.types.size(), -1);
    result.enum_of_type.resize_to_filled(m.types.size(), -1);
    result.array_of_type.resize_to_filled(m.types.size(), -1);

    auto p = planner{.p = result};
    // a tessellation stage's control points take the first locations, and its factors struct's members the next
    if (check::is_valid(e.input) && m.at(e.input).kind == type_kind::array
        && (e.entry_stage == stage::tessellation_control || e.entry_stage == stage::tessellation_evaluation))
        for (auto const& member : m.at(m.at(m.at(e.input).element).members))
            p.patch_location_base += member.is_position ? 0 : 1;
    p.need(e.input, input_role(e));
    p.need(e.result, result_role(e));
    // the geometry stage's stream, and the evaluation stage's factors, are parameters after the first
    for (auto const& local : e.locals)
    {
        if (local.kind != check::local_kind::parameter)
            continue;
        auto const& t = m.at(local.type);
        if (t.kind == type_kind::stream)
            p.need(t.element, struct_role::stage_link);
        if (e.entry_stage == stage::tessellation_evaluation && t.kind == type_kind::structure && local.type != e.input)
            p.need(local.type, struct_role::patch_constants);
    }
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
    // A struct in a block or a buffer is declared whether or not the code reads it whole.
    for (auto const id : e.bindings)
        for (auto const& member : m.at(m.bindings[m.at(id).info].members))
        {
            auto const& whole = m.at(member.type);
            auto const& t = m.takes_slots(member.type) && whole.kind == type_kind::array ? m.at(whole.element) : whole;
            p.need(t.kind == type_kind::buffer ? t.element : member.type, struct_role::plain);
        }
    // `spell` is what mints `<name>_` where the target reserves the name or a builtin is called by it.
    result.entry_name = p.spell(e.name);
    if (e.entry_stage == stage::tessellation_control)
    {
        result.patch_function = result.names.mint(cc::format("{}_patch", e.name));
        result.point_index = result.names.mint("point_index");
    }
    p.constants();
    p.group_blocks();
    p.resources();
    p.argument_buffers();
    p.workgroup_memory();
    p.file_samplers();
    p.memory_forms();
    p.struct_offsets();
    // The check pass minted the locals, so a buffer or a block minted above never took one's name.
    for (auto const& local : e.locals)
        result.locals.push_back(p.spell(local.name));
    for (auto const& input : e.stage_inputs)
    {
        auto const& local = result.locals[index_of(input.local)];
        result.stage_input_names.push_back(result.names.mint(cc::format("{}_in", local)));
        result.stage_input_bases.push_back(result.names.mint(cc::format("{}_base", local)));
    }
    return result;
}

cc::vector<cc::string> sgl::emit::impl::vertex_semantics(check::checked_module const& m, check::type_info const& t)
{
    auto result = cc::vector<cc::string>();
    for (auto const& member : m.at(t.members))
    {
        auto semantic = cc::string(member.name);
        for (auto& c : semantic.as_mutable_span())
            if (c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
        // HLSL reads a trailing number as the semantic's index, and dx12 refuses a name that ends in one
        if (!semantic.empty() && semantic.back() >= '0' && semantic.back() <= '9')
            semantic += '_';
        // a name that another member took, ignoring case, moves on rather than colliding
        auto const taken = [&]
        {
            for (auto const& other : result)
                if (other == semantic)
                    return true;
            return false;
        };
        while (taken())
            semantic += '_';
        result.push_back(cc::move(semantic));
    }
    return result;
}

bool sgl::emit::impl::has_base(plan const& p, check::stage_input input)
{
    auto const is_hlsl = p.which == target::hlsl_dx12 || p.which == target::hlsl_vulkan;
    return is_hlsl && (input == check::stage_input::vertex_index || input == check::stage_input::instance_index);
}

sgl::emit::impl::stage_input_spelling const& sgl::emit::impl::spelling_of(check::stage_input input)
{
    using check::stage_input;
    static constexpr stage_input_spelling k_vertex_index
        = {"uint", "SV_VertexID", "u32", "vertex_index", "uint", "vertex_id"};
    static constexpr stage_input_spelling k_instance_index
        = {"uint", "SV_InstanceID", "u32", "instance_index", "uint", "instance_id"};
    static constexpr stage_input_spelling k_front_facing
        = {"bool", "SV_IsFrontFace", "bool", "front_facing", "bool", "front_facing"};
    static constexpr stage_input_spelling k_sample_index
        = {"uint", "SV_SampleIndex", "u32", "sample_index", "uint", "sample_id"};
    static constexpr stage_input_spelling k_sample_mask
        = {"uint", "SV_Coverage", "u32", "sample_mask", "uint", "sample_mask"};
    static constexpr stage_input_spelling k_primitive_id
        = {"uint", "SV_PrimitiveID", "u32", "primitive_index", "uint", "primitive_id"};
    static constexpr stage_input_spelling k_thread_id
        = {"uint3", "SV_DispatchThreadID", "vec3u", "global_invocation_id", "uint3", "thread_position_in_grid"};
    static constexpr stage_input_spelling k_local_thread_id
        = {"uint3", "SV_GroupThreadID", "vec3u", "local_invocation_id", "uint3", "thread_position_in_threadgroup"};
    static constexpr stage_input_spelling k_local_thread_index
        = {"uint", "SV_GroupIndex", "u32", "local_invocation_index", "uint", "thread_index_in_threadgroup"};
    static constexpr stage_input_spelling k_workgroup_id
        = {"uint3", "SV_GroupID", "vec3u", "workgroup_id", "uint3", "threadgroup_position_in_grid"};
    // its HLSL type is the domain's, which the parameter states: `float3` for triangles, `float2` otherwise
    static constexpr stage_input_spelling k_domain_location
        = {"float3", "SV_DomainLocation", "vec3f", "", "float3", ""};
    switch (input)
    {
    case stage_input::vertex_index:
        return k_vertex_index;
    case stage_input::instance_index:
        return k_instance_index;
    case stage_input::is_front_facing:
        return k_front_facing;
    case stage_input::sample_index:
        return k_sample_index;
    case stage_input::sample_mask:
        return k_sample_mask;
    case stage_input::primitive_id:
        return k_primitive_id;
    case stage_input::thread_id:
        return k_thread_id;
    case stage_input::local_thread_id:
        return k_local_thread_id;
    case stage_input::local_thread_index:
        return k_local_thread_index;
    case stage_input::workgroup_id:
        return k_workgroup_id;
    case stage_input::domain_location:
        return k_domain_location;
    // a ray-tracing stage reads its launch through builtins, which flatten binds its parameters to
    case stage_input::launch_id:
    case stage_input::launch_size:
    case stage_input::none:
        break;
    }
    CC_UNREACHABLE("a stage input without a spelling");
}

cc::string sgl::emit::impl::stage_input_value(plan const& p, isize index)
{
    auto const& input = p.e.stage_inputs[index];
    auto const& raw = p.stage_input_names[index];
    auto const type = check::info_of(input.input).type;
    auto const is_wgsl = p.which == target::wgsl;
    // HLSL counts a vertex and an instance from the draw's base, and DXC keeps that meaning on vulkan (EMIT-128).
    auto const value
        = has_base(p, input.input) ? cc::format("{} + {}", raw, p.stage_input_bases[index]) : cc::string(raw);
    if (type == "int")
        return cc::format("{}({})", is_wgsl ? "i32" : "int", value);
    if (type == "int3")
        return cc::format("{}({})", is_wgsl ? "vec3i" : "int3", value);
    return value;
}

namespace
{
/// The leaves of `members` placed in `space`, each named as the text spells the members on its way down.
/// `member_of` maps a member to its position in `planned`, as a planned struct's does; empty where they are parallel.
cc::vector<emitted_field> fields_of(plan const& p,
                                    cc::span<planned_member const> planned,
                                    cc::span<i32 const> member_of,
                                    cc::span<member_info const> members,
                                    address_space space)
{
    auto result = cc::vector<emitted_field>();
    for (auto const& leaf : place(p.m, members, space).leaves)
    {
        auto const top = member_of.empty() ? leaf.path[0] : member_of[leaf.path[0]];
        auto name = cc::string(planned[top].name);
        auto type = members[leaf.path[0]].type;
        for (auto i = isize(1); i < leaf.path.size(); ++i)
        {
            auto const& s = p.structs[p.struct_of_type[index_of(type)]];
            name.appendf(".{}", s.members[s.member_of[leaf.path[i]]].name);
            type = p.m.at(p.m.at(type).members)[leaf.path[i]].type;
        }
        result.push_back({.name = cc::move(name), .offset = leaf.offset});
    }
    return result;
}

cc::vector<emitted_field> fields_of(memory_form const& form)
{
    auto result = cc::vector<emitted_field>();
    for (auto const& f : form.fields)
        result.push_back({.name = f.name, .offset = f.offset});
    return result;
}
} // namespace

cc::vector<sgl::emit::emitted_layout> sgl::emit::impl::layouts_of(plan const& p)
{
    auto result = cc::vector<emitted_layout>();
    auto const block = [&](planned_constants const& c)
    {
        auto const plain = plain_members_of(p.m, p.m.bindings[p.m.at(c.symbol).info]);
        result.push_back({.global = c.name,
                          .fields = c.form.has_value() ? fields_of(c.form.value())
                                                       : fields_of(p, c.members, {}, plain, address_space::constants)});
    };
    if (p.constants.has_value())
        block(p.constants.value());
    for (auto const& c : p.group_blocks)
        block(c);
    for (auto const& r : p.resources)
    {
        if (p.m.at(r.type).kind != type_kind::buffer)
            continue;
        auto layout = emitted_layout{.global = r.name, .stride = element_stride(p.m, r.element)};
        if (r.element_form.has_value())
            layout.fields = fields_of(r.element_form.value());
        else if (auto const at = p.struct_of_type[index_of(r.element)]; at >= 0)
        {
            auto const& s = p.structs[at];
            layout.fields
                = fields_of(p, s.members, s.member_of, p.m.at(p.m.at(r.element).members), address_space::storage);
        }
        result.push_back(cc::move(layout));
    }
    return result;
}
