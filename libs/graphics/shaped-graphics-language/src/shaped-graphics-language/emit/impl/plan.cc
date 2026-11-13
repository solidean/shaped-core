#include "plan.hh"

#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/emit/impl/layout.hh>
#include <shaped-graphics-language/emit/reserved_words.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// Whether `type` holds an `atomic[float]`, as itself, as an array's element or as a struct's field.
[[nodiscard]] bool holds_float_atomic(checked_module const& m, type_id type)
{
    auto const& t = m.at(type);
    if (t.kind == type_kind::atomic)
        return m.name_of(t.element) == builtins::k_float;
    if (t.kind == type_kind::array)
        return holds_float_atomic(m, t.element);
    if (t.kind == type_kind::structure && !is_builtin_type(m, type))
        for (auto const& field : m.at(t.members))
            if (holds_float_atomic(m, field.type))
                return true;
    return false;
}

/// Whether the entry point's workgroup memory holds an `atomic[float]`.
[[nodiscard]] bool shares_float_atomic(checked_module const& m, flat_entry_point const& e)
{
    for (auto const id : e.bindings)
    {
        auto const& b = m.bindings[m.at(id).info];
        if (b.is_workgroup)
            for (auto const& member : m.at(b.members))
                if (holds_float_atomic(m, member.type))
                    return true;
    }
    return false;
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
    case struct_role::patch_constants:
        return "factors struct";
    case struct_role::primitive_link:
        return "per-triangle struct";
    }
    return "";
}

/// True for a name some builtin writes in this target: a local of that name would hide it.
/// Read from the registry, so a new builtin needs no entry in a target's list of reserved words.
bool is_called_by_a_builtin(checked_module const& m, emit::target t, cc::string_view name)
{
    return m.builtins != nullptr && m.builtins->names_written_in(language_of(t)).contains(name);
}

struct_role input_role(flat_entry_point const& e)
{
    // A compute parameter crosses no edge: it is a system value, or a struct of them.
    // a ray-tracing stage's payload is a plain struct the target hands over by reference
    // a mesh stage's payload is a plain struct, which its task stage stored in groupshared memory
    if (e.entry_stage == stage::compute || e.entry_stage == stage::task || e.entry_stage == stage::mesh
        || e.entry_stage >= stage::raygen)
        return struct_role::plain;
    // the geometry and the tessellation stages take an array of what the stage before hands on
    return e.entry_stage == stage::vertex ? struct_role::vertex_input : struct_role::stage_link;
}

struct_role result_role(flat_entry_point const& e)
{
    // an intersection's report is a value its traversal reads back, never a render target
    if (e.entry_stage >= stage::raygen)
        return struct_role::plain;
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
    bool writes_prints = false;

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
            if (s.node.is<flat_print>() && !writes_prints)
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
                for (auto const id : e.reserved)
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
            else if (auto const* c = x.node.try_as<flat_call>(); c != nullptr && c->text >= 0)
            {
                // CHK-427: a body in a target's own text takes its function's parameters, each in order
                auto const parameters = m.at(m.functions[m.at(c->callee).info].parameters).size();
                if (c->text >= m.target_texts.size() || parameters != e.at(c->arguments).size())
                    report(error_kind::malformed_tree, e.function,
                           cc::format("a target's text of '{}' with {} arguments", m.at(c->callee).name,
                                      e.at(c->arguments).size()));
            }
            else if (auto const* c = x.node.try_as<flat_call>())
            {
                auto const* const record = m.builtin_function(c->intrinsic);
                auto const expected = record == nullptr ? -1
                                                        : record->parameters.size() + (record->takes_element ? 1 : 0)
                                                              + (record->takes_acceleration_index ? 1 : 0)
                                                              + (record->takes_ray_time ? 1 : 0);
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
    /// The names the bindings of the tree were spelled, so a second binding of one name is minted one of its own.
    cc::set<cc::string> binding_spellings;
    /// Where a factors struct's locations start: after the control point's, which SPIR-V counts in the same space.
    i32 patch_location_base = 0;
    /// Where a per-triangle struct's locations start: after the stage link's, whose space the pixel stage shares.
    i32 primitive_location_base = 0;

    /// How many locations the members of a stage link `type` take.
    [[nodiscard]] i32 locations_of(type_id type) const
    {
        auto count = 0;
        for (auto const& member : p.m.at(p.m.at(type).members))
            if (!member.is_position && !member.is_layer && member.type != check::checked_module::void_type)
            {
                count += locations_taken(member.type);
            }
        return count;
    }

    /// A name of the program as this target may spell it: itself, or with a trailing underscore where it is reserved.
    /// Every target reserves the prefix `sgl_` for what an emitter writes, whose names end in no underscore.
    cc::string spell(cc::string_view name)
    {
        if (!is_reserved(p.which, name) && !is_called_by_a_builtin(p.m, p.which, name) && !name.starts_with("sgl_"))
            return name;
        return p.names.mint(cc::format("{}_", name));
    }

    /// `spell` for a binding's or a file-scope sampler's name, which two modules may each declare (CHK-350): the second
    /// the tree meets is minted a name of its own.
    cc::string spell_binding(cc::string_view name)
    {
        auto result = spell(name);
        if (binding_spellings.contains(result))
            result = p.names.mint(result);
        binding_spellings.insert(result);
        return result;
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
            // EMIT-138: a member named like the struct type of another hides the type from it in HLSL and MSL.
            // The type is compared as written, which a reserved name escapes: `ray` is `ray_` on msl, like the member.
            for (auto const& s : siblings)
            {
                if (!is_valid(s.type) || p.m.at(s.type).kind != type_kind::structure)
                    continue;
                if (p.m.name_of(s.type) == result)
                    return false;
                if (auto const k = p.struct_of_type[index_of(s.type)]; k >= 0 && p.structs[k].name == result)
                    return false;
            }
            if (result == name)
                return true;
            for (auto const& s : siblings)
                if (s.name == result)
                    return false;
            return true;
        };
        // one trailing underscore, then a number after it: a second underscore would make a name C++ reserves
        for (auto suffix = 0; !is_free(); ++suffix)
            result = suffix == 0 ? cc::format("{}_", name) : cc::format("{}_{}", name, suffix);
        return result;
    }

    cc::vector<planned_member> members_of(ast::range_of<member_info> range, bool has_locations)
    {
        return members_of(p.m.at(range), has_locations);
    }

    /// EMIT-174: the element of a one-dimensional array of builtins, or the column of a matrix, and how many cross.
    struct edge_pieces
    {
        type_id piece = type_id::none;
        i32 count = 0;
        i32 piece_locations = 1;
    };

    [[nodiscard]] type_id prelude_type_named(cc::string_view name) const
    {
        for (auto i = isize(0); i < p.m.types.size(); ++i)
            if (auto const type = type_id(i32(i)); p.m.builtin_type_of(type) != nullptr && p.m.name_of(type) == name)
                return type;
        return type_id::none;
    }

    [[nodiscard]] edge_pieces pieces_of(type_id type) const
    {
        if (!is_valid(type) || type == check::checked_module::error_type)
            return {};
        if (auto const* const record = p.m.builtin_type_of(type))
        {
            auto const name = p.m.name_of(type);
            if (name == "mat2" || name == "mat3" || name == "mat4")
            {
                auto const n = name == "mat2" ? 2 : name == "mat3" ? 3 : 4;
                return {.piece = prelude_type_named(n == 2 ? "float2" : n == 3 ? "float3" : "float4"), .count = n};
            }
            if (auto const words = words_of(type); is_valid(words))
                return {.piece = words, .count = word_pieces(p, type)};
            return {};
        }
        auto const& t = p.m.at(type);
        if (t.kind == type_kind::array && t.count > 0 && p.m.builtin_type_of(t.element) != nullptr)
        {
            auto const words = words_of(t.element);
            return {.piece = is_valid(words) ? words : t.element,
                    .count = i32(t.count) * (is_valid(words) ? word_pieces(p, t.element) : 1),
                    .piece_locations = !is_valid(words) && is_wide_64_bit(p, t.element) ? 2 : 1};
        }
        return {};
    }

    /// EMIT-196: the uint2 or uint4 a 64-bit value crosses a link as where the target carries no 64-bit value between
    /// stages, a uint4 apiece for a three- or four-vector; none elsewhere.
    [[nodiscard]] type_id words_of(type_id type) const
    {
        auto const* const record = p.m.builtin_type_of(type);
        if (record == nullptr || !is_word_split(p, type))
            return type_id::none;
        return prelude_type_named(record->leaf_count == 1 ? "uint2" : "uint4");
    }

    /// CHK-454: the locations a member of `type` takes on a link, a 64-bit three- or four-vector two of them.
    [[nodiscard]] i32 locations_taken(type_id type) const
    {
        if (auto const split = pieces_of(type); split.count > 0)
            return split.count * split.piece_locations;
        return is_wide_64_bit(p, type) ? 2 : 1;
    }

    cc::vector<planned_member> members_of(cc::span<member_info const> members, bool has_locations, i32 first_location = 0)
    {
        auto result = cc::vector<planned_member>();
        auto next_location = first_location;
        for (auto const& member : members)
        {
            auto const split = has_locations && !member.is_position && !member.is_layer
                                    && member.factor == check::tessellation_factor::none
                                 ? pieces_of(member.type)
                                 : edge_pieces();
            result.push_back({
                .name = spell_member(member.name, members),
                .source_name = member.name,
                .type = member.type,
                .is_position = has_locations && member.is_position,
                .is_layer = has_locations && member.is_layer,
                .interpolate = member.interpolate,
                .output = member.output,
                .factor = member.factor,
                .location = has_locations && !member.is_position && !member.is_layer
                                 && member.output == check::pixel_output::color
                                 && member.factor == check::tessellation_factor::none
                              ? next_location
                              : -1,
                .is_row_per_element = member.padding == check::padding_kind::every_element,
                .pieces = split.count,
                .piece = split.piece,
                .piece_locations = split.piece_locations,
            });
            if (result.back().location >= 0)
                next_location += locations_taken(member.type);
        }
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
    /// `report[hit_attributes]` as a name every target takes: `report_hit_attributes`, minted.
    cc::string instance_name(type_id type)
    {
        // a closing bracket is dropped, since every one ends the name or stands before another
        auto name = cc::string();
        // a constant argument such as `-1.5` is spelled with what no identifier holds, so each such character is `_`
        for (auto const c : p.m.name_of(type))
            if (c != ']' && c != ' ')
                name += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? c : '_';
        return p.names.mint(name);
    }

    void need(type_id type, struct_role role)
    {
        // a pixel stage that returns nothing has no result to declare (CHK-418)
        if (type == check::checked_module::void_type)
            return;
        // a binding array is declared with its resource, and has no type of its own to spell
        if (is_valid(type) && !is_builtin_type(p.m, type) && p.m.at(type).kind == type_kind::array)
            return p.m.takes_slots(type) ? void() : need_array(type, role);
        if (!is_valid(type) || is_builtin_type(p.m, type) || p.m.at(type).kind != type_kind::structure)
            return;
        if (p.struct_of_type[index_of(type)] != -1)
            return;
        // CHK-432: a text type this compile spells in its target's text is the target's own, which nothing declares
        for (auto const& t : p.e.text_types)
            if (t.type == type)
                return;
        auto const& info = p.m.at(type);
        // CHK-339: two instances of one template whose members came out alike are one struct, since after the front end
        // nothing tells them apart
        if (is_valid(info.generic))
            for (auto i = isize(0); i < p.structs.size(); ++i)
            {
                auto const& other = p.m.at(p.structs[i].type);
                if (other.generic != info.generic || p.structs[i].role != role
                    || other.members.count != info.members.count)
                    continue;
                auto const mine = p.m.at(info.members);
                auto const theirs = p.m.at(other.members);
                auto is_alike = true;
                for (auto k = isize(0); is_alike && k < mine.size(); ++k)
                    is_alike = mine[k] == theirs[k];
                if (is_alike)
                {
                    p.struct_of_type[index_of(type)] = i32(i);
                    return;
                }
            }
        // Claimed before the members are visited; the check pass reports a struct that holds itself, so this is no loop.
        p.struct_of_type[index_of(type)] = -2;

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
            .name = is_valid(info.generic) || is_valid(info.tangent_of) ? instance_name(type)
                                                                        : spell_type(p.m.at(info.symbol).name),
            .role = role,
            .members = members_of(written, role != struct_role::plain,
                                  role == struct_role::patch_constants  ? patch_location_base
                                  : role == struct_role::primitive_link ? primitive_location_base
                                                                        : 0),
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
                                       .host_name = cc::format("{}.{}", p.m.qualified_name_of(id), members[i].name),
                                       .type = is_array ? whole.element : members[i].type,
                                       .element = t.element,
                                       .space = element_space(p.m.bindings[s.info], members[i]),
                                       .is_mut = t.is_mut,
                                       .is_coherent = members[i].is_coherent,
                                       .group = group,
                                       .slot = slot,
                                       .count = count});
                slot += count;
            }
            ++group;
        }
    }

    /// What legalize reached of the reserved table, at its fixed address, behind every group's own.
    void reserved_resources()
    {
        for (auto const id : p.e.reserved)
        {
            auto const& s = p.m.at(id);
            auto const* const entry = check::reserved_binding_named(s.name);
            CC_ASSERT(entry != nullptr, "the tree reaches a binding the reserved table does not hold");
            auto const& on = entry->targets[u8(p.which)];
            auto const& b = p.m.bindings[s.info];
            // an entry of plain members is a constant block of its own, sg's root constants or uniform
            if (auto const plain = plain_members_of(p.m, b); !plain.empty())
            {
                auto planned = planned_constants{
                    .symbol = id,
                    .name = cc::string(s.name),
                    .host_name = cc::string(s.name),
                    .block_name = p.names.mint(cc::format("{}_data", s.name)),
                    .members = members_of(plain, false),
                    .group = on.group,
                    .slot = on.slot,
                    .is_reserved = true,
                };
                auto const placed = place_block(p.m, plain, block_space(b));
                for (auto i = isize(0); i < planned.members.size(); ++i)
                {
                    planned.members[i].offset = placed.offsets[i];
                    planned.members[i].size = placed.sizes[i];
                }
                for (auto i = isize(0); i < plain.size(); ++i)
                    planned.block_member_of.push_back(i32(i));
                p.group_blocks.push_back(cc::move(planned));
                continue;
            }
            auto const& member = p.m.at(b.members)[0];
            auto const& t = p.m.at(member.type);
            p.resources.push_back({.binding = id,
                                   .member = 0,
                                   .name = s.name,
                                   .type = member.type,
                                   .element = t.element,
                                   .space = address_space::storage,
                                   .is_mut = t.is_mut,
                                   .is_reserved = true,
                                   .group = on.group,
                                   .slot = on.slot});
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
                    .name = spell_binding(s.name),
                    .host_name = p.m.qualified_name_of(id),
                    .block_name = p.names.mint(cc::format("{}_data", s.name)),
                    .members = members_of(plain, false),
                    .group = group,
                    .slot = 0,
                };
                auto const placed = place_block(p.m, plain, block_space(b));
                for (auto i = isize(0); i < planned.members.size(); ++i)
                {
                    planned.members[i].offset = placed.offsets[i];
                    planned.members[i].size = placed.sizes[i];
                }
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
            p.samplers.insert_at(at, {.symbol = smp->sampler,
                                      .name = spell_binding(s.name),
                                      .host_name = p.m.qualified_name_of(smp->sampler),
                                      .type = s.type,
                                      .index = index});
        }
    }

    /// What WGSL and MSL need to reach SGL's layout, where their own rule would not (memory_form.hh).
    void memory_forms()
    {
        auto const form_of_block = [&](planned_constants& block)
        {
            auto const& b = p.m.bindings[p.m.at(block.symbol).info];
            block.form = memory_form_of(p.m, plain_members_of(p.m, b), block_space(b), 0, p.which);
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
            r.element_form = element_form_of(p.m, r.element, r.space, p.which);
            if (r.element_form.has_value())
                r.element_form.value().name = p.names.mint(cc::format("{}_memory", p.m.name_of(r.element)));
        }
        // an array of structs in a form is spelled as the struct is planned, and an array of arrays as the inner one is
        auto const spell_structs = [&](cc::optional<memory_form>& form)
        {
            if (form.has_value())
                for (auto& f : form.value().fields)
                    if (is_valid(f.struct_type) && p.struct_of_type[index_of(f.struct_type)] >= 0)
                        f.type = p.structs[p.struct_of_type[index_of(f.struct_type)]].name;
                    else if (is_valid(f.struct_type) && p.array_of_type[index_of(f.struct_type)] >= 0)
                        f.type = p.array_texts[p.array_of_type[index_of(f.struct_type)]];
        };
        if (p.constants.has_value())
            spell_structs(p.constants.value().form);
        for (auto& block : p.group_blocks)
            spell_structs(block.form);
        for (auto& r : p.resources)
            spell_structs(r.element_form);
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
                    offsets_of(t.element, element_space(b, member));
                else if (!check::is_resource(t.kind))
                    offsets_of(member.type, block_space(b));
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
            planned.memory_size = placed.size;
            planned.is_packed = is_packed(space);
            planned.is_constants = !is_storage(space);
            planned.memory_alignment = placed.alignment;
            for (auto i = isize(0); i < planned.member_of.size(); ++i)
                if (planned.member_of[i] >= 0)
                {
                    planned.members[planned.member_of[i]].offset = placed.offsets[i];
                    planned.members[planned.member_of[i]].size = placed.sizes[i];
                }
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
                .name = spell_binding(s.name),
                .host_name = p.m.qualified_name_of(id),
                .block_name = p.names.mint(cc::format("{}_data", s.name)),
                .members = members_of(b.members, false),
            };
            auto const placed = place_block(p.m, p.m.at(b.members), block_space(b));
            for (auto i = isize(0); i < planned.members.size(); ++i)
            {
                planned.members[i].offset = placed.offsets[i];
                planned.members[i].size = placed.sizes[i];
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
    // a sampler of a module the program does not use takes no index, as it would not exist beside its modules alone
    auto index = 0;
    for (auto i = isize(0); i < index_of(symbol); ++i)
        index += m.symbols[i].kind == check::symbol_kind::sampler && m.is_reached_file(m.symbols[i].file) ? 1 : 0;
    return index;
}

sgl::i32 sgl::emit::impl::resource_of(plan const& p, check::symbol_id binding, i32 member)
{
    for (auto i = isize(0); i < p.resources.size(); ++i)
        if (p.resources[i].binding == binding && p.resources[i].member == member)
            return i32(i);
    return -1;
}

sgl::u32 sgl::emit::impl::acceleration_address_position(plan const& p, i32 index)
{
    auto const& r = p.resources[index];
    auto position = u32(0);
    for (auto const& other : p.resources)
        if (!other.is_reserved && p.m.at(other.type).kind == check::type_kind::acceleration_structure
            && (other.group < r.group || (other.group == r.group && other.slot < r.slot)))
            ++position;
    return position;
}

bool sgl::emit::impl::holds_acceleration_member(plan const& p)
{
    for (auto const& r : p.resources)
        if (!r.is_reserved && p.m.at(r.type).kind == check::type_kind::acceleration_structure)
            return true;
    return false;
}

cc::string_view sgl::emit::impl::atomic_text(plan const& p, check::type_id type)
{
    // a float's atomic is a uint's, holding its bits, unless the target's own float atomic is in use
    auto const element = p.m.name_of(p.m.at(type).element);
    if (element == builtins::k_float && p.native_float_atomics)
        return p.which == target::msl ? "atomic_float" : "float";
    auto const is_signed = element == builtins::k_int;
    // a 64-bit atomic is HLSL's, and a ulong's MSL's for its min and max (EMIT-192, EMIT-218)
    if (element == builtins::k_long)
        return p.which == target::msl ? "atomic_long" : "int64_t";
    if (element == builtins::k_ulong)
        return p.which == target::msl ? "atomic_ulong" : "uint64_t";
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
        // EMIT-174: a stage link carries an array of what crosses, and a matrix, as one member per element or column
        auto const is_link = role == struct_role::stage_link || role == struct_role::patch_constants
                          || role == struct_role::primitive_link || role == struct_role::vertex_input;
        auto const* const element = record == nullptr && m.at(member.type).kind == check::type_kind::array
                                      ? m.builtin_type_of(m.at(member.type).element)
                                      : nullptr;
        auto const is_matrix
            = record != nullptr
           && (m.name_of(member.type) == "mat2" || m.name_of(member.type) == "mat3" || m.name_of(member.type) == "mat4");
        auto const is_carried = is_link && (is_matrix || (element != nullptr && element->crosses_edges));
        if ((record == nullptr || !record->crosses_edges) && !is_factor && !is_carried)
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

    // EMIT-154: every target reads a packed block from its memory form, and HLSL has no expression that builds a
    // struct value out of the fields
    if (block_space(b) == address_space::packed_constants)
        for (auto const& member : plain_members_of(m, b))
            if (member.type != check::checked_module::void_type && m.builtin_type_of(member.type) == nullptr
                && m.at(member.type).kind == check::type_kind::structure)
                report(error_kind::unsupported,
                       cc::format("a struct member in a @padding(.none) block: '{}.{}'", s.name, member.name));
    // C++ aligns a `tg::vec4f` at 4, and no target declares an array anywhere but at the start of a row
    if (block_space(b) == address_space::packed_constants)
    {
        auto const plain = plain_members_of(m, b);
        auto const placed = place(m, plain, address_space::packed_constants);
        for (auto i = isize(0); i < plain.size(); ++i)
            if (plain[i].type != check::checked_module::void_type && m.builtin_type_of(plain[i].type) == nullptr
                && m.at(plain[i].type).kind == check::type_kind::array && placed.offsets[i] % 16 != 0)
                report(error_kind::unsupported, cc::format("an array at byte {} of a @padding(.none) block, where C++ "
                                                           "places it and no target can: '{}.{}'",
                                                           placed.offsets[i], s.name, plain[i].name));
    }

    // EMIT-175: HLSL's constant buffer starts each column of a packed block's mat2 on a row, where C++ packs it
    auto const holds_mat2 = [&](auto const& self, check::type_id type) -> bool
    {
        if (type == check::checked_module::void_type || is_mat2(m, type))
            return is_mat2(m, type);
        if (m.builtin_type_of(type) != nullptr)
            return false;
        auto const& t = m.at(type);
        if (t.kind == check::type_kind::array || t.kind == check::type_kind::buffer)
            return self(self, t.element);
        if (t.kind == check::type_kind::structure)
            for (auto const& member : m.at(t.members))
                if (member.type != type && self(self, member.type))
                    return true;
        return false;
    };
    for (auto const& member : m.at(b.members))
    {
        auto const& t = m.at(member.type);
        auto const space = t.kind == check::type_kind::buffer ? element_space(b, member) : block_space(b);
        if (!m.takes_slots(member.type) || t.kind == check::type_kind::buffer)
            if (space == address_space::packed_constants && holds_mat2(holds_mat2, member.type))
                report(error_kind::unsupported,
                       cc::format("a mat2 in @padding(.none) memory: '{}.{}'", s.name, member.name));
    }

    // CHK-291's arrays that no target places yet, refused alike on every target
    auto const judge_arrays = [&](auto const& self, check::type_id type, address_space space, cc::string const& where,
                                  bool is_block_member) -> void
    {
        if (type == check::checked_module::void_type || m.builtin_type_of(type) != nullptr)
            return;
        auto const& t = m.at(type);
        if (t.kind == check::type_kind::array)
        {
            auto const element = t.element;
            // EMIT-154 places a packed block's array of whole rows, and EMIT-178 a buffer element's array of builtins
            if (is_packed(space) && m.builtin_type_of(element) == nullptr)
                report(error_kind::unsupported, cc::format("an array in @padding(.none) memory: {}", where));
            else if (space == address_space::packed_constants && !is_block_member)
                report(error_kind::unsupported, cc::format("an array in @padding(.none) memory: {}", where));
            else
                self(self, element, space, where, false);
            return;
        }
        if (t.kind == check::type_kind::structure)
            for (auto const& member : m.at(t.members))
                if (member.type != type)
                    self(self, member.type, space, cc::format("{}.{}", where, member.name), false);
    };
    for (auto const& member : m.at(b.members))
    {
        auto const& whole = m.at(member.type);
        auto const& t = m.takes_slots(member.type) && whole.kind == check::type_kind::array ? m.at(whole.element) : whole;
        auto const where = cc::format("'{}.{}'", s.name, member.name);
        if (member.padding == check::padding_kind::every_element && block_space(b) == address_space::packed_constants)
            report(error_kind::unsupported, cc::format("@padding(.every_element) in a @padding(.none) block: {}", where));
        if (t.kind == check::type_kind::buffer)
            judge_arrays(judge_arrays, t.element, element_space(b, member), where, false);
        else if (!m.takes_slots(member.type))
            judge_arrays(judge_arrays, member.type, block_space(b), where, true);
    }

    // EMIT-114: a struct has one layout, so every binding places it by one rule; two rules would give it two.
    constexpr address_space spaces[] = {address_space::constants, address_space::storage,
                                        address_space::packed_constants, address_space::packed_storage};
    for (auto const space : spaces)
    {
        auto mine = cc::vector<check::type_id>();
        collect_placed_structs(m, id, space, mine);
        for (auto const type : mine)
        {
            auto is_reported = false;
            for (auto const other : spaces)
            {
                if (other == space || is_reported)
                    continue;
                for (auto const& info : m.bindings)
                {
                    if (!m.is_reached_file(m.at(info.symbol).file))
                        continue;
                    auto theirs = cc::vector<check::type_id>();
                    collect_placed_structs(m, info.symbol, other, theirs);
                    if (!contains(theirs, type))
                        continue;
                    // Worded from the lower rule's side, so the binding on either side finds the same error.
                    auto const is_first = space < other;
                    report_on_struct(
                        error_kind::layout_conflict, type,
                        cc::format("'{}' is in a {} of '{}' and in a {} of '{}'", m.name_of(type),
                                   space_name(is_first ? space : other), is_first ? s.name : m.at(info.symbol).name,
                                   space_name(is_first ? other : space), is_first ? m.at(info.symbol).name : s.name));
                    is_reported = true;
                    break;
                }
            }
        }
    }

    auto const plain = plain_members_of(m, b);
    if (b.is_no_padding)
        for (auto& gap : padding_of(m, plain, block_space(b)))
            report(error_kind::padding_forbidden, cc::format("in the block of @no_padding '{}': {}", s.name, gap));
    for (auto const space : spaces)
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

cc::vector<sgl::emit::impl::planned_member> sgl::emit::impl::declared_members(planned_struct const& s)
{
    auto result = cc::vector<planned_member>();
    for (auto const& member : s.members)
    {
        if (member.pieces == 0)
        {
            result.push_back(member);
            result.back().whole = &member - s.members.data();
            continue;
        }
        for (auto i = 0; i < member.pieces; ++i)
        {
            auto piece = member;
            piece.whole = &member - s.members.data();
            piece.piece_index = i;
            piece.name = cc::format("{}_{}", member.name, i);
            piece.source_name = cc::format("{}_{}", member.source_name, i);
            piece.type = member.piece;
            piece.location = member.location + i * member.piece_locations;
            piece.pieces = 0;
            result.push_back(cc::move(piece));
        }
    }
    return result;
}

bool sgl::emit::impl::shares_tail(plan const& p, planned_struct const& s, isize member)
{
    return s.memory_size > 0 && !s.is_packed && shares_tail(p, cc::span<planned_member const>(s.members), member);
}

bool sgl::emit::impl::shares_tail(plan const& p, cc::span<planned_member const> members, isize member)
{
    if (member < 0 || member + 1 >= members.size() || members[member].offset < 0)
        return false;
    auto const* const record = p.m.builtin_type_of(members[member].type);
    return record != nullptr && record->leaf_count == 3
        && members[member + 1].offset < members[member].offset + record->msl_layout.size;
}

cc::string_view sgl::emit::impl::row_vector_of(plan const& p, check::type_id element)
{
    switch (p.m.builtin_type_of(element)->leaf_kind)
    {
    case check::value_kind::scalar_int:
        return "int4";
    case check::value_kind::scalar_uint:
        return "uint4";
    case check::value_kind::scalar_half:
        return "half4";
    case check::value_kind::scalar_short:
        return "short4";
    case check::value_kind::scalar_ushort:
        return "ushort4";
    // a row of 16 bytes holds two 64-bit scalars (EMIT-191)
    case check::value_kind::scalar_long:
        return "long2";
    case check::value_kind::scalar_ulong:
        return "ulong2";
    case check::value_kind::scalar_double:
        return "double2";
    default:
        return "float4";
    }
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
                                                              cc::span<check::member_info const> members,
                                                              address_space space)
{
    auto placed = place(m, members, space);
    return {.offsets = cc::move(placed.offsets), .sizes = cc::move(placed.sizes), .size = placed.end};
}

void sgl::emit::impl::validate(check::checked_module const& m,
                               check::flat_entry_point const& e,
                               cc::vector<error>& errors,
                               bool writes_prints)
{
    auto v = validator{.m = m, .e = e, .errors = errors, .writes_prints = writes_prints};
    // a mesh stage made from a geometry stage takes no payload and returns nothing, as a compute stage does
    if (e.input == e.result && e.entry_stage != stage::compute && e.entry_stage != stage::task
        && !(e.entry_stage == stage::mesh && !check::is_valid(e.input)))
        v.report(error_kind::unsupported, e.function,
                 cc::format("one struct as both the parameter and the result: '{}'", m.name_of(e.input)));

    // A compute entry point has no pipeline edge at either end, so neither struct is judged as one, and a ray-tracing
    // stage's payload is handed over by reference rather than across an edge.
    if (e.entry_stage != stage::compute && e.entry_stage != stage::task && e.entry_stage < stage::raygen)
    {
        // the geometry and the tessellation stages take an array of what crosses, and a geometry stage hands its
        // vertices on through its stream rather than its result
        auto input = e.input;
        if (check::is_valid(input) && m.at(input).kind == type_kind::array)
            input = m.at(input).element;
        if (check::is_valid(input))
            v.edge_struct(input, input_role(e));
        if (e.entry_stage == stage::geometry || e.entry_stage == stage::mesh)
        {
            for (auto const& local : e.locals)
                if (local.kind == check::local_kind::parameter && m.at(local.type).kind == type_kind::stream)
                    v.edge_struct(m.at(local.type).element, struct_role::stage_link);
        }
        // a pixel stage that writes images alone returns nothing, and has no render target (CHK-418)
        else if (input != e.result && e.result != check::checked_module::void_type)
            v.edge_struct(e.result, result_role(e));
    }
    v.bindings();
    v.tree();
    v.file_samplers();
}

namespace
{
/// What the body reaches, which is all a text writes: the legalizer leaves the nodes it replaced behind, unreachable.
struct live_nodes
{
    check::flat_entry_point const& e;
    cc::vector<bool> exprs;
    cc::vector<bool> locals;

    void local(check::local_id id)
    {
        if (check::impl::is_known(e, id))
            locals[check::index_of(id)] = true;
    }

    void expr(check::flat_expr_id id, int depth)
    {
        if (!check::impl::is_known(e, id) || depth > check::impl::k_max_depth || exprs[check::index_of(id)])
            return;
        exprs[check::index_of(id)] = true;
        auto const& x = e.at(id);
        if (auto const* const ref = x.node.try_as<check::flat_local_ref>())
            local(ref->local);
        if (auto const* const b = x.node.try_as<check::flat_block>())
            body(b->body, depth + 1);
        check::impl::for_each_operand(e, x, [&](check::flat_expr_id operand) { expr(operand, depth + 1); });
    }

    void body(ast::range_of<check::flat_stmt_id> range, int depth)
    {
        if (!check::impl::is_known(e, range) || depth > check::impl::k_max_depth)
            return;
        for (auto const id : e.at(range))
        {
            if (!check::impl::is_known(e, id))
                continue;
            auto const& s = e.at(id);
            if (auto const* const let = s.node.try_as<check::flat_let>())
                local(let->local);
            else if (auto const* const var = s.node.try_as<check::flat_var>())
                local(var->local);
            else if (auto const* const f = s.node.try_as<check::flat_for>())
                local(f->index);
            check::impl::for_each_expr_of(e, s, [&](check::flat_expr_id x) { expr(x, depth + 1); });
            check::impl::for_each_pattern_of(e, s,
                                             [&](ast::range_of<check::flat_expr_id> patterns)
                                             {
                                                 if (check::impl::is_known(e, patterns))
                                                     for (auto const p : e.at(patterns))
                                                         expr(p, depth + 1);
                                             });
            check::impl::for_each_body_of(e, s,
                                          [&](ast::range_of<check::flat_stmt_id> inner) { body(inner, depth + 1); });
        }
    }
};
} // namespace

sgl::emit::impl::plan sgl::emit::impl::make_plan(check::checked_module const& m,
                                                 check::flat_entry_point const& e,
                                                 target t,
                                                 bool native_float_atomic_add)
{
    auto result = plan{.m = m, .e = e, .which = t};
    // dx12 and WebGPU have no float add, so there an `atomic[float]` stays a uint's bits whatever the device says.
    // MSL has float atomics in threadgroup memory only from 4.1, past sg's floor of 4.0 (the spec's bindings file).
    // So an entry point sharing one keeps every `atomic[float]` a uint's bits, the type being one whatever holds it.
    result.native_float_atomics
        = native_float_atomic_add && (t == target::hlsl_vulkan || (t == target::msl && !shares_float_atomic(m, e)));
    // The tree's names and the builtins' go on being taken where they are, since both outlive the plan.
    result.names.parent = &e.names;
    if (m.builtins != nullptr)
        result.names.also_taken = &m.builtins->names_written_in(language_of(t));
    // so nothing minted below can be a reserved word
    result.names.also_reserved = &reserved_set(t);
    result.struct_of_type.resize_to_filled(m.types.size(), -1);
    result.enum_of_type.resize_to_filled(m.types.size(), -1);
    result.array_of_type.resize_to_filled(m.types.size(), -1);

    auto p = planner{.p = result};
    // a tessellation stage's control points take the first locations, and its factors struct's members the next
    if (check::is_valid(e.input) && m.at(e.input).kind == type_kind::array
        && (e.entry_stage == stage::tessellation_control || e.entry_stage == stage::tessellation_evaluation))
        p.patch_location_base = p.locations_of(m.at(e.input).element);
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
        // CHK-488, CHK-489: what a mesh stage writes per triangle, and the pixel stage's struct of it, after the link
        if (t.kind == type_kind::stream && check::is_valid(t.primitive))
        {
            p.primitive_location_base = p.locations_of(t.element);
            p.need(t.primitive, struct_role::primitive_link);
        }
        if (e.entry_stage == stage::pixel && t.kind == type_kind::structure && local.type != e.input
            && check::is_valid(e.input))
        {
            p.primitive_location_base = p.locations_of(e.input);
            p.need(local.type, struct_role::primitive_link);
        }
        if (e.entry_stage == stage::tessellation_evaluation && t.kind == type_kind::structure && local.type != e.input)
            p.need(local.type, struct_role::patch_constants);
    }
    // Only what the body reaches: a struct the legalizer split into its fields is held by nothing the text writes.
    auto live = live_nodes{.e = e,
                           .exprs = cc::vector<bool>::create_filled(e.exprs.size(), false),
                           .locals = cc::vector<bool>::create_filled(e.locals.size(), false)};
    live.body(e.body, 0);
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        if (!live.locals[i] && local.kind != check::local_kind::parameter)
            continue;
        p.need(local.type, struct_role::plain);
        p.need_enum(local.type);
    }
    for (auto i = isize(0); i < e.exprs.size(); ++i)
    {
        if (!live.exprs[i])
            continue;
        p.need(e.exprs[i].type, struct_role::plain);
        p.need_enum(e.exprs[i].type);
    }
    // EMIT-139: MSL sizes the ray data by every payload of the set, which each of its shaders must agree on
    if (t == target::msl)
        if (auto const set = ray_set_of(m, e); check::is_valid(set))
            for (auto const& ray : m.at(m.at(m.at(set).type).members))
                p.need(ray.type, struct_role::plain);
    // A struct in a block or a buffer is declared whether or not the code reads it whole.
    for (auto const id : e.bindings)
        for (auto const& member : m.at(m.bindings[m.at(id).info].members))
        {
            auto const& whole = m.at(member.type);
            auto const& t = m.takes_slots(member.type) && whole.kind == type_kind::array ? m.at(whole.element) : whole;
            p.need(t.kind == type_kind::buffer ? t.element : member.type, struct_role::plain);
        }
    // `spell` is what mints `<name>_` where the target reserves the name or a builtin is called by it.
    // An entry point the check pass adds for metal is named `sgl_…` on purpose, and the host asks for it by that name.
    auto const is_added = check::is_valid(e.function) && m.at(e.function).name != e.name;
    result.entry_name = is_added ? cc::string(e.name) : p.spell(e.name);
    if (e.entry_stage == stage::tessellation_control)
    {
        result.patch_function = result.names.mint(cc::format("{}_patch", e.name));
        result.point_index = result.names.mint("point_index");
    }
    p.constants();
    p.group_blocks();
    p.resources();
    p.reserved_resources();
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
    // the parameter stays in the signature, and the line that converts it is written where the tree reads it
    result.stage_input_read = cc::vector<bool>::create_filled(e.stage_inputs.size(), false);
    for (auto const& x : e.exprs)
        if (auto const* const ref = x.node.try_as<check::flat_local_ref>())
            for (auto i = isize(0); i < e.stage_inputs.size(); ++i)
                if (e.stage_inputs[i].local == ref->local)
                    result.stage_input_read[i] = true;

    result.local_flags = cc::vector<cc::optional<u32>>::create_defaulted(e.locals.size());
    auto flags_known = cc::vector<bool>::create_filled(e.locals.size(), true);

    // A trace hands its flags on through a local more often than as a literal: one given a literal and never assigned
    // again holds that literal wherever it is read.
    auto constant_locals = cc::vector<check::flat_int_literal const*>::create_filled(e.locals.size(), nullptr);
    for (auto const& st : e.stmts)
    {
        auto local = check::local_id::none;
        auto value = check::flat_expr_id::none;
        if (auto const* const let = st.node.try_as<check::flat_let>())
        {
            local = let->local;
            value = let->value;
        }
        else if (auto const* const var = st.node.try_as<check::flat_var>())
        {
            local = var->local;
            value = var->value;
        }
        if (check::is_valid(value))
            constant_locals[index_of(local)] = e.at(value).node.try_as<check::flat_int_literal>();
    }
    for (auto const& st : e.stmts)
        if (auto const* const assign = st.node.try_as<check::flat_assign>())
            if (auto const* const ref = e.at(assign->place).node.try_as<check::flat_local_ref>())
                constant_locals[index_of(ref->local)] = nullptr;
    auto const literal_of = [&](check::flat_expr_id x) -> check::flat_int_literal const*
    {
        if (auto const* const ref = e.at(x).node.try_as<check::flat_local_ref>())
            return constant_locals[index_of(ref->local)];
        return e.at(x).node.try_as<check::flat_int_literal>();
    };
    for (auto const& x : e.exprs)
    {
        auto const* const call = x.node.try_as<check::flat_call>();
        auto const* const record = call != nullptr ? m.builtin_function(call->intrinsic) : nullptr;
        if (record == nullptr || record->flags_argument < 0)
            continue;
        auto const arguments = e.at(call->arguments);
        auto const* const target = e.at(arguments[0]).node.try_as<check::flat_local_ref>();
        if (target == nullptr)
            continue;
        auto const at = index_of(target->local);
        auto const* const k = literal_of(arguments[record->flags_argument]);
        auto& flags = result.local_flags[at];
        if (k == nullptr || (flags.has_value() && flags.value() != u32(k->value)))
            flags_known[at] = false;
        else
            flags = u32(k->value);
    }
    for (auto i = isize(0); i < result.local_flags.size(); ++i)
        if (!flags_known[i])
            result.local_flags[i] = cc::nullopt;
    plan_memory_accesses(result);
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

cc::string sgl::emit::impl::semantic_of(plan const& p, planned_struct const& s, planned_member const& member)
{
    if (member.is_position)
        return "SV_Position";
    if (member.is_layer)
        return "SV_RenderTargetArrayIndex";
    switch (s.role)
    {
    case struct_role::vertex_input:
    {
        // EMIT-174: a piece is its member's semantic at its own index, `FRAME0`, which is how dx12 reads one
        auto const at = member.whole >= 0 ? member.whole : &member - s.members.data();
        auto const semantics = vertex_semantics(p.m, p.m.at(s.type));
        for (auto i = isize(0); i < s.member_of.size(); ++i)
            if (s.member_of[i] == at)
                return member.piece_index >= 0 ? cc::format("{}{}", semantics[i], member.piece_index) : semantics[i];
        return "";
    }
    case struct_role::stage_link:
    case struct_role::primitive_link:
        return cc::format("SGL{}", member.location);
    case struct_role::patch_constants:
        // EMIT-123: the factors are the tessellator's, and any other member reaches the evaluation stage as a link
        if (member.factor == check::tessellation_factor::edge)
            return "SV_TessFactor";
        if (member.factor == check::tessellation_factor::inside)
            return "SV_InsideTessFactor";
        return cc::format("SGL{}", member.location);
    case struct_role::render_targets:
        // EMIT-130: the depth and the sample mask are outputs of their own
        switch (member.output)
        {
        case check::pixel_output::depth:
            return "SV_Depth";
        case check::pixel_output::depth_greater_equal:
            return "SV_DepthGreaterEqual";
        case check::pixel_output::depth_less_equal:
            return "SV_DepthLessEqual";
        case check::pixel_output::sample_mask:
            return "SV_Coverage";
        case check::pixel_output::color:
            break;
        }
        return cc::format("SV_Target{}", member.location);
    case struct_role::plain:
        break;
    }
    return "";
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
    // WGSL has no builtin for it, which the feature it needs keeps from ever reaching that target
    static constexpr stage_input_spelling k_first_instance
        = {"uint", "SV_StartInstanceLocation", "u32", "", "uint", "base_instance"};
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
    static constexpr stage_input_spelling k_subgroup_size
        = {"uint", "", "u32", "subgroup_size", "uint", "threads_per_simdgroup", "WaveGetLaneCount()"};
    static constexpr stage_input_spelling k_subgroup_invocation_id
        = {"uint", "", "u32", "subgroup_invocation_id", "uint", "thread_index_in_simdgroup", "WaveGetLaneIndex()"};
    // WGSL has none, which view_instancing keeps from that target; Metal's is its vertex amplification's
    static constexpr stage_input_spelling k_view_index = {"uint", "SV_ViewID", "u32", "", "uint", "amplification_id"};
    switch (input)
    {
    case stage_input::vertex_index:
        return k_vertex_index;
    case stage_input::instance_index:
        return k_instance_index;
    case stage_input::first_instance:
        return k_first_instance;
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
    case stage_input::subgroup_size:
        return k_subgroup_size;
    case stage_input::subgroup_invocation_id:
        return k_subgroup_invocation_id;
    case stage_input::view_index:
        return k_view_index;
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
    auto const is_wgsl = p.which == target::wgsl;
    auto const is_hlsl = p.which == target::hlsl_dx12 || p.which == target::hlsl_vulkan;
    // EMIT-147: an input HLSL has no semantic for is read where the entry point starts
    auto const raw = is_hlsl && !spelling_of(input.input).hlsl_read.empty()
                       ? spelling_of(input.input).hlsl_read
                       : cc::string_view(p.stage_input_names[index]);
    auto const type = check::info_of(input.input).type;
    // HLSL counts a vertex and an instance from the draw's base, and DXC keeps that meaning on vulkan (EMIT-128).
    // An entry taking `@first_instance` too has that base as a parameter already, and HLSL takes one semantic once.
    auto base = cc::string(p.stage_input_bases[index]);
    if (input.input == check::stage_input::instance_index)
        for (auto j = isize(0); j < p.e.stage_inputs.size(); ++j)
            if (p.e.stage_inputs[j].input == check::stage_input::first_instance)
                base = cc::string(p.stage_input_names[j]);
    auto const value = has_base(p, input.input) ? cc::format("{} + {}", raw, base) : cc::string(raw);
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
        auto const& b = p.m.bindings[p.m.at(c.symbol).info];
        auto const plain = plain_members_of(p.m, b);
        result.push_back({.global = c.name,
                          .fields = c.form.has_value() ? fields_of(c.form.value())
                                                       : fields_of(p, c.members, {}, plain, block_space(b))});
    };
    if (p.constants.has_value())
        block(p.constants.value());
    for (auto const& c : p.group_blocks)
        if (!c.is_reserved)
            block(c);
    for (auto const& r : p.resources)
    {
        if (p.m.at(r.type).kind != type_kind::buffer || r.is_reserved)
            continue;
        auto layout = emitted_layout{.global = r.name, .stride = element_stride(p.m, r.element, r.space)};
        if (r.element_form.has_value())
            layout.fields = fields_of(r.element_form.value());
        else if (auto const at = p.struct_of_type[index_of(r.element)]; at >= 0)
        {
            auto const& s = p.structs[at];
            layout.fields = fields_of(p, s.members, s.member_of, p.m.at(p.m.at(r.element).members), r.space);
        }
        result.push_back(cc::move(layout));
    }
    return result;
}

cc::vector<sgl::check::symbol_id> sgl::emit::impl::owning_ray_sets(check::checked_module const& m,
                                                                   check::flat_entry_point const& e)
{
    auto result = cc::vector<check::symbol_id>();
    auto const is_in = [](cc::span<check::symbol_id const> ids, check::symbol_id id)
    {
        for (auto const x : ids)
            if (x == id)
                return true;
        return false;
    };
    auto const add = [&](check::symbol_id set)
    {
        if (check::is_valid(set) && !is_in(result, set))
            result.push_back(set);
    };
    auto const holds = [&](check::pipeline_info const& p)
    {
        switch (p.kind)
        {
        case check::pipeline_kind::raytracing:
            // every callable of the module's tables is in each ray-tracing pipeline's callable section
            return p.raygen == e.function || is_in(m.at(p.misses), e.function) || e.entry_stage == check::stage::callable;
        case check::pipeline_kind::hit_group:
            return p.intersection == e.function || is_in(m.at(p.records), e.function);
        default:
            return false;
        }
    };
    for (auto const& p : m.pipelines)
        if (holds(p))
            add(p.ray_set);
    return result;
}

sgl::check::symbol_id sgl::emit::impl::ray_set_of(check::checked_module const& m, check::flat_entry_point const& e)
{
    if (e.entry_stage < check::stage::raygen)
        return check::symbol_id::none;
    // the pipeline or the hit group that holds the entry point decides, so every shader linked with it agrees
    if (auto const owners = owning_ray_sets(m, e); !owners.empty())
        return owners[0];
    if (!e.traced_rays.empty())
        return e.traced_rays.front().set;
    if (e.entry_stage == check::stage::callable || !check::is_valid(e.input))
        return check::symbol_id::none;
    for (auto const& p : m.pipelines)
    {
        if (!check::is_valid(p.ray_set) || !m.is_reached_file(m.at(p.symbol).file))
            continue;
        for (auto const& ray : m.at(m.at(m.at(p.ray_set).type).members))
            if (ray.type == e.input)
                return p.ray_set;
    }
    return check::symbol_id::none;
}
