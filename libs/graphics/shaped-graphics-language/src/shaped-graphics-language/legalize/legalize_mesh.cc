#include <clean-core/common/assertf.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-524: a mesh stage on a target without one is a compute pass and a draw.
// The compute form writes each workgroup's counts, indices, vertices and primitives into fixed slots of
// `sg_mesh_output`, and the vertex form reads them back from `sg_mesh_input`, one invocation a corner of every
// primitive slot in order, so the draw keeps the primitive order the native stage has.
// A primitive's values ride on each of its corners, flat, since the unindexed draw runs every corner on its own.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// The mesh output parameter of `e`, whose type is `mesh_output[V]` or `mesh_output[V, P]`.
[[nodiscard]] local_id output_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        auto const& t = m.at(local.type);
        if (local.kind == local_kind::parameter && t.kind == type_kind::stream && t.count == 0)
            return local_id(i);
    }
    return local_id::none;
}

/// What a call of a mesh output's method writes: 0 for `set_counts`, 1 for `vertex`, 2 for `triangle`, 3 for
/// `primitive` and 4 for `line`; -1 for every other expression.
[[nodiscard]] i32 mesh_write_of(checked_module const& m, flat_expr const& x)
{
    auto const* const call = x.node.try_as<flat_call>();
    auto const* const record = call == nullptr ? nullptr : m.builtin_function(call->intrinsic);
    if (record == nullptr)
        return -1;
    constexpr cc::string_view names[] = {"set_counts", "vertex", "triangle", "primitive", "line"};
    for (auto i = 0; i < 5; ++i)
        if (record->name == names[i])
            return i;
    return -1;
}

/// The number `g` of the launched mesh workgroup this one of the indirect dispatch is, which owns slots `g`.
/// Its task workgroup is the last whose first launched is at most `g`, which a binary search over the records finds.
/// The workgroup id it reads is `g`'s place in that task workgroup's grid, and its payload is the record's.
[[nodiscard]] flat_expr_id launched_workgroup(checked_module const& m,
                                              flat_builder& out,
                                              words& w,
                                              task_polyfill_layout const& task,
                                              stmt_list& into)
{
    using t = task_polyfill_layout;
    auto in = words{.out = out, .binding = listed(out, "sg_task_input")};
    auto const int3 = out.type_named("int3");
    auto const uint = w.uint_type();

    // the workgroup id the body reads is the launched one's, and the dispatch's is a stage input of its own
    auto user_group = local_id::none;
    for (auto i = isize(0); i < out.e.stage_inputs.size(); ++i)
        if (out.e.stage_inputs[i].input == stage_input::workgroup_id)
        {
            user_group = out.e.stage_inputs[i].local;
            out.e.locals[index_of(user_group)].kind = local_kind::temporary;
            out.e.stage_inputs.remove_at(i);
            break;
        }
    auto const dispatch = stage_input_of(out, stage_input::workgroup_id, "mesh_dispatch_id", int3);
    auto const g = w.bound("mesh_launched", w.unary("convert_int_to_uint", out.member(out.local(dispatch), 0)), into);

    auto const record_of = [&](flat_expr_id index)
    {
        return w.binary("add_uint", w.word(u32(t::k_first_record_word)),
                        w.binary("multiply_uint", index, w.word(u32(task.record_words()))));
    };

    // the last record whose first launched is at most `g`; a record that launched none shares its first with the next
    auto const low = out.add_local(local_kind::temporary, "mesh_task_low", uint);
    auto const high = out.add_local(local_kind::temporary, "mesh_task_high", uint);
    into.push_back(out.var(low, w.word(0)));
    into.push_back(out.var(high, w.binary("subtract_uint", in.at(w.word(u32(t::k_task_count_word))), w.word(1))));
    auto step = stmt_list();
    auto const middle = w.bound(
        "mesh_task_middle",
        w.binary("divide_uint", w.binary("add_uint", w.binary("add_uint", out.local(low), out.local(high)), w.word(1)),
                 w.word(2)),
        step);
    flat_stmt_id const up[] = {out.assign(out.local(low), middle)};
    flat_stmt_id const down[] = {out.assign(out.local(high), w.binary("subtract_uint", middle, w.word(1)))};
    step.push_back(out.if_(w.binary("less_equal_uint", in.at(record_of(middle), u32(t::k_record_first)), g), up, down));
    into.push_back(
        out.while_(out.add_label("mesh_task_search"), w.binary("less_uint", out.local(low), out.local(high)), step));

    auto const record = w.bound("mesh_task_record", record_of(out.local(low)), into);
    auto const j = w.bound("mesh_in_task", w.binary("subtract_uint", g, in.at(record, u32(t::k_record_first))), into);
    auto const gx = w.bound("mesh_task_gx", in.at(record, u32(t::k_record_grid)), into);
    auto const gy = w.bound("mesh_task_gy", in.at(record, u32(t::k_record_grid + 1)), into);
    if (is_valid(user_group))
    {
        auto const to_int = [&](flat_expr_id v) { return w.unary("convert_uint_to_int", v); };
        flat_expr_id const lanes[] = {
            to_int(w.binary("remainder_uint", j, gx)),
            to_int(w.binary("remainder_uint", w.binary("divide_uint", j, gx), gy)),
            to_int(w.binary("divide_uint", j, w.binary("multiply_uint", gx, gy))),
        };
        into.push_back(out.let(user_group, out.construct(int3, lanes)));
    }

    // the payload parameter is the record's
    for (auto i = isize(0); i < out.e.locals.size(); ++i)
    {
        auto is_input = false;
        for (auto const& s : out.e.stage_inputs)
            is_input = is_input || index_of(s.local) == i;
        auto const type = out.e.locals[i].type;
        if (out.e.locals[i].kind != local_kind::parameter || is_input || m.at(type).kind != type_kind::structure)
            continue;
        out.e.locals[i].kind = local_kind::temporary;
        auto const payload_at
            = w.bound("mesh_payload_at", w.binary("add_uint", record, w.word(u32(t::k_record_payload))), into);
        auto next = isize(0);
        into.push_back(out.let(local_id(i), in.read_value(type, payload_at, next)));
        break;
    }
    return g;
}

/// A pixel stage's second struct, what a mesh stage wrote per primitive (CHK-489); `none` where it takes none.
[[nodiscard]] local_id primitive_parameter_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(1); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        auto is_input = false;
        for (auto const& input : e.stage_inputs)
            is_input = is_input || index_of(input.local) == i;
        if (local.kind == local_kind::parameter && !is_input && m.at(local.type).kind == type_kind::structure
            && local.type != e.input)
            return local_id(i);
    }
    return local_id::none;
}

} // namespace

cc::result<mesh_polyfill_layout, cc::string> sgl::check::mesh_polyfill_layout_of(checked_module const& m,
                                                                                 flat_entry_point const& e,
                                                                                 bool is_launched)
{
    CC_ASSERT(e.entry_stage == stage::mesh, "only a mesh entry point has a polyfill layout");
    auto const output = output_of(m, e);
    if (!is_valid(output))
        return cc::error(cc::format("{} takes no mesh output", e.name));
    auto const& t = m.at(e.at(output).type);
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto is_input = false;
        for (auto const& input : e.stage_inputs)
            is_input = is_input || index_of(input.local) == i;
        if (e.locals[i].kind != local_kind::parameter || local_id(i) == output || is_input)
            continue;
        if (!is_launched)
            return cc::error(
                cc::format("{} takes a task stage's payload, and no polyfilled task stage launches it", e.name));
        if (auto const payload = leaves_of(m, e.locals[i].type); payload.has_error())
            return cc::error(cc::format("{}'s payload holds a '{}', which no word holds", e.name, payload.error()));
    }
    auto const leaves = leaves_of(m, t.element);
    if (leaves.has_error())
        return cc::error(cc::format("{}'s vertex holds a '{}', which no word holds", e.name, leaves.error()));

    auto primitive_words = i32(0);
    if (is_valid(t.primitive))
    {
        auto const primitive_leaves = leaves_of(m, t.primitive);
        if (primitive_leaves.has_error())
            return cc::error(
                cc::format("{}'s primitive holds a '{}', which no word holds", e.name, primitive_leaves.error()));
        primitive_words = i32(primitive_leaves.value().size());
    }

    auto const& info = m.functions[m.at(e.function).info];
    auto layout = mesh_polyfill_layout{.corners = info.is_line_mesh ? 2 : 3,
                                       .max_vertices = info.max_vertices,
                                       .max_primitives = info.max_primitives,
                                       .vertex_words = i32(leaves.value().size()),
                                       .primitive_words = primitive_words};
    layout.group_words = layout.first_primitive_word() + layout.max_primitives * layout.primitive_words;
    return layout;
}

flat_entry_point sgl::check::mesh_as_compute(checked_module const& m,
                                             flat_entry_point const& e,
                                             task_polyfill_layout const* task)
{
    auto const layout = mesh_polyfill_layout_of(m, e, task != nullptr);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    auto const output = output_of(m, e);
    auto const vertex_type = m.at(e.at(output).type).element;
    auto const leaves = leaves_of(m, vertex_type).value();
    auto const primitive_type = m.at(e.at(output).type).primitive;
    auto const primitive_leaves = is_valid(primitive_type) ? leaves_of(m, primitive_type).value() : cc::vector<leaf>();

    auto out = flat_builder::extend(m, e);
    auto w = words{.out = out, .binding = listed(out, "sg_mesh_output")};
    auto prologue = stmt_list();
    auto linear = flat_expr_id::none;
    if (task == nullptr)
    {
        // `base = (x + nx * (y + ny * z)) * group_words`, the grid being the threads sg dispatched
        auto const group = stage_input_of(out, stage_input::workgroup_id, "workgroup_id", out.type_named("int3"));
        linear = linear_workgroup(w, group, e.workgroup, prologue);
    }
    else
        linear = launched_workgroup(m, out, w, *task, prologue);
    auto const base
        = w.bound("mesh_base", w.binary("multiply_uint", linear, w.word(u32(layout.value().group_words))), prologue);

    auto const replace = [&](flat_stmt_id sid, stmt_list& into) -> bool
    {
        auto const* const eval = out.e.at(sid).node.try_as<flat_eval>();
        if (eval == nullptr || !is_valid(eval->value))
            return false;
        auto const write = mesh_write_of(m, out.e.at(eval->value));
        if (write < 0)
            return false;
        auto const& s = out.e.at(sid);
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto const arguments = cc::vector<flat_expr_id>::create_copy_of(
            cc::span<flat_expr_id const>(out.e.at(out.e.at(eval->value).node.as<flat_call>().arguments)));
        auto const store = [&](flat_expr_id first, u32 offset, flat_expr_id value)
        { into.push_back(out.assign(w.at(first, offset), value)); };
        if (write == 0)
        {
            store(base, 0, arguments[1]);
            store(base, 1, arguments[2]);
            return true;
        }
        auto const corners = u32(layout.value().corners);
        if (write == 2 || write == 4)
        {
            auto const indices = w.bound("mesh_indices", arguments[2], into);
            auto const first = w.bound("mesh_at",
                                       w.binary("add_uint", base,
                                                w.binary("add_uint", w.word(u32(layout.value().first_index_word())),
                                                         w.binary("multiply_uint", arguments[1], w.word(corners)))),
                                       into);
            for (auto c = u32(0); c < corners; ++c)
                store(first, c, out.member(indices, i32(c)));
            return true;
        }
        // a vertex or a primitive: its leaves in field order
        auto const is_primitive = write == 3;
        auto const& written = is_primitive ? primitive_leaves : leaves;
        auto const value = w.bound(is_primitive ? "mesh_primitive" : "mesh_vertex", arguments[2], into);
        auto const first_word = is_primitive ? layout.value().first_primitive_word() : layout.value().first_vertex_word();
        auto const stride = is_primitive ? layout.value().primitive_words : layout.value().vertex_words;
        auto const first = w.bound("mesh_at",
                                   w.binary("add_uint", base,
                                            w.binary("add_uint", w.word(u32(first_word)),
                                                     w.binary("multiply_uint", arguments[1], w.word(u32(stride))))),
                                   into);
        for (auto k = isize(0); k < written.size(); ++k)
            store(first, u32(k), w.encode(w.leaf_of(value, written[k]), written[k].kind));
        return true;
    };

    auto body = stmt_list::create_copy_of(cc::span<flat_stmt_id const>(prologue));
    auto const rest = rewritten(out, out.e.body, replace);
    for (auto const s : out.e.at(rest))
        body.push_back(s);
    out.e.locals[index_of(output)].kind = local_kind::temporary;
    // the output or the payload, standing first, is no stage struct of a compute stage
    out.e.input = type_id::none;
    out.e.entry_stage = stage::compute;
    out.e.features.remove(feature::mesh_shader);
    // the slots' base and the grid are read by every store, which the compaction gives a node each
    return compacted(m, out.e, body);
}

flat_entry_point sgl::check::mesh_as_vertex_fetch(checked_module const& m, flat_entry_point const& e, type_id corner)
{
    auto const layout = mesh_polyfill_layout_of(m, e, true);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    CC_ASSERT(layout.value().primitive_words == 0 || is_valid(corner), "a mesh stage that writes values per primitive "
                                                                       "hands them on in add_mesh_corner's struct");
    return slots_as_vertex_fetch(m, e, layout.value(), m.at(e.at(output_of(m, e)).type).element, corner);
}

sgl::check::mesh_corner_parts sgl::check::mesh_corner_parts_of(checked_module const& m, flat_entry_point const& e)
{
    if (e.entry_stage == stage::mesh)
    {
        auto const output = output_of(m, e);
        if (!is_valid(output))
            return {};
        auto const& t = m.at(e.at(output).type);
        return {.vertex = t.element, .primitive = t.primitive};
    }
    if (e.entry_stage != stage::pixel || !is_valid(e.input))
        return {};
    auto const primitive = primitive_parameter_of(m, e);
    return {.vertex = e.input, .primitive = is_valid(primitive) ? e.at(primitive).type : type_id::none};
}

type_id sgl::check::add_mesh_corner(checked_module& m, mesh_corner_parts parts)
{
    CC_ASSERT(is_valid(parts.vertex) && is_valid(parts.primitive), "a mesh corner is of a vertex and a primitive");
    auto const vertex_members = cc::vector<member_info>::create_copy_of(m.at(m.at(parts.vertex).members));
    auto const primitive_members = cc::vector<member_info>::create_copy_of(m.at(m.at(parts.primitive).members));
    auto const first = u32(m.members.size());
    for (auto const& member : vertex_members)
        m.members.push_back(member);
    // a primitive's value is the same at each of its corners, so nothing interpolates it; its name differs from the
    // vertex's, since the locations alone link it to the pixel stage
    for (auto member : primitive_members)
    {
        auto is_taken = true;
        while (is_taken)
        {
            is_taken = false;
            for (auto const& v : vertex_members)
                is_taken = is_taken || v.name == member.name;
            if (is_taken)
                member.name = cc::format("{}_of_primitive", member.name);
        }
        member.interpolate = {.kind = interpolation::kind_t::flat};
        member.has_interpolate = true;
        m.members.push_back(cc::move(member));
    }

    auto const vertex_symbol = m.at(m.at(parts.vertex).symbol);
    auto const symbol = symbol_id(m.symbols.size());
    auto const type = type_id(m.types.size());
    m.symbols.push_back({.file = vertex_symbol.file,
                         .declaration = vertex_symbol.declaration,
                         .kind = symbol_kind::structure,
                         .state = symbol_state::checked,
                         .name = "sgl_mesh_corner",
                         .type = type});
    m.types.push_back({.kind = type_kind::structure,
                       .symbol = symbol,
                       .members = {.first = first, .count = u32(vertex_members.size() + primitive_members.size())}});
    return type;
}

flat_entry_point sgl::check::pixel_after_mesh_polyfill(checked_module const& m, flat_entry_point const& e, type_id corner)
{
    CC_ASSERT(e.entry_stage == stage::pixel && is_valid(e.input), "only a pixel stage takes a primitive struct");
    auto const primitive = primitive_parameter_of(m, e);
    CC_ASSERT(is_valid(primitive), "the pixel stage takes a primitive struct");

    auto out = flat_builder::extend(m, e);
    if (e.body.count > 0)
        out.from = e.at(e.at(e.body)[0]).from;
    // `locals[0]` is the stage struct, now the corner: what read the vertex struct reads the local made from it
    auto const vertex_type = e.input;
    auto const vertex = out.add_local(local_kind::temporary, e.locals[0].name, vertex_type);
    for (auto& x : out.e.exprs)
        if (auto* const ref = x.node.try_as<flat_local_ref>(); ref != nullptr && index_of(ref->local) == 0)
            ref->local = vertex;
    out.e.locals[0].type = corner;
    out.e.locals[index_of(primitive)].kind = local_kind::temporary;
    out.e.input = corner;

    auto body = stmt_list();
    auto const vertex_count = isize(m.at(vertex_type).members.count);
    auto const corner_count = isize(m.at(corner).members.count);
    auto vertex_parts = cc::vector<flat_expr_id>();
    auto primitive_parts = cc::vector<flat_expr_id>();
    for (auto k = isize(0); k < corner_count; ++k)
        (k < vertex_count ? vertex_parts : primitive_parts).push_back(out.member(out.local(local_id(0)), i32(k)));
    body.push_back(out.let(vertex, out.construct(vertex_type, vertex_parts)));
    body.push_back(out.let(primitive, out.construct(e.at(primitive).type, primitive_parts)));
    for (auto const s : out.e.at(out.e.body))
        body.push_back(s);
    return compacted(m, out.e, body);
}
