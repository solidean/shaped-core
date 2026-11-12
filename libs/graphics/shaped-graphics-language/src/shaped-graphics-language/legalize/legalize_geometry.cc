#include <clean-core/common/assertf.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-525: a geometry stage on a target without one is a compute pass and a draw.
// One thread a primitive assembles it from the vertex buffers, runs the vertex stage on each corner and the geometry
// stage on the primitive, and writes what the stage appends into the primitive's fixed slots of `sg_mesh_output`.
// The mesh polyfill's vertex form draws them back, so the slots keep the native order: primitive by primitive, then
// in the order each appended.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// The stream parameter of a geometry entry point `e`: `point_stream[T]`, `line_stream[T]` or `triangle_stream[T]`.
[[nodiscard]] local_id stream_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        auto const& t = m.at(local.type);
        if (local.kind == local_kind::parameter && t.kind == type_kind::stream && t.count > 0)
            return local_id(i);
    }
    return local_id::none;
}

[[nodiscard]] bool is_stage_input(flat_entry_point const& e, local_id local)
{
    for (auto const& input : e.stage_inputs)
        if (input.local == local)
            return true;
    return false;
}

/// The primitive parameter of a geometry entry point `e`, an array of the vertex stage's result.
[[nodiscard]] local_id primitive_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        if (local.kind == local_kind::parameter && m.at(local.type).kind == type_kind::array
            && !is_stage_input(e, local_id(i)))
            return local_id(i);
    }
    return local_id::none;
}

/// What a call of a stream's method writes: 0 for `emit`, 1 for `end_strip`; -1 for every other expression.
[[nodiscard]] i32 stream_write_of(checked_module const& m, flat_expr const& x)
{
    auto const* const call = x.node.try_as<flat_call>();
    auto const* const record = call == nullptr ? nullptr : m.builtin_function(call->intrinsic);
    if (record == nullptr)
        return -1;
    if (record->name == "emit")
        return 0;
    if (record->name == "end_strip")
        return 1;
    return -1;
}

} // namespace

cc::result<mesh_polyfill_layout, cc::string> sgl::check::geometry_polyfill_layout_of(checked_module const& m,
                                                                                     flat_entry_point const& g)
{
    CC_ASSERT(g.entry_stage == stage::geometry, "only a geometry entry point has a geometry polyfill layout");
    auto const stream = stream_of(m, g);
    if (!is_valid(stream))
        return cc::error(cc::format("{} takes no stream", g.name));
    auto const& t = m.at(g.at(stream).type);
    if (t.count == 1)
        return cc::error(cc::format("{} appends points, which the polyfill does not draw yet", g.name));
    auto const leaves = leaves_of(m, t.element);
    if (leaves.has_error())
        return cc::error(cc::format("{}'s vertex holds a '{}', which no word holds", g.name, leaves.error()));
    auto const& info = m.functions[m.at(g.function).info];
    auto layout = mesh_polyfill_layout{.corners = t.count,
                                       .max_vertices = info.max_vertices,
                                       .max_primitives = cc::max(1, info.max_vertices - (t.count - 1)),
                                       .vertex_words = i32(leaves.value().size())};
    layout.group_words = layout.first_vertex_word() + layout.max_vertices * layout.vertex_words;
    return layout;
}

cc::result<flat_entry_point, cc::string> sgl::check::geometry_as_compute(checked_module const& m,
                                                                         flat_entry_point const& v,
                                                                         flat_entry_point const& g)
{
    CC_ASSERT(v.entry_stage == stage::vertex, "the stage before a polyfilled geometry stage is a vertex stage");
    auto const layout_or = geometry_polyfill_layout_of(m, g);
    if (layout_or.has_error())
        return cc::error(layout_or.error());
    auto const layout = layout_or.value();
    auto const primitive = primitive_of(m, g);
    auto const stream = stream_of(m, g);
    if (!is_valid(primitive))
        return cc::error(cc::format("{} takes no primitive", g.name));
    auto const primitive_type = g.at(primitive).type;
    auto const corners = m.at(primitive_type).count;
    auto const leaves = leaves_of(m, m.at(g.at(stream).type).element).value();

    auto vertex_input = cc::optional<vertex_layout>();
    if (is_valid(v.input))
    {
        auto read = vertex_layout_of(m, v.input);
        if (read.has_error())
            return cc::error(read.error());
        vertex_input = cc::move(read.value());
    }
    for (auto const& input : v.stage_inputs)
        if (input.input == stage_input::view_index)
            return cc::error(cc::format("{} reads @view_index, which the polyfill does not carry yet", v.name));

    auto out = flat_builder::extend(m, g);
    if (g.body.count > 0)
        out.from = g.at(g.at(g.body)[0]).from;
    out.inlined_through = {};
    auto const from = out.from;
    auto w = words{.out = out, .binding = listed(out, "sg_mesh_output")};
    auto const draw = listed(out, "sg_geometry_draw");
    auto const draw_value = [&](i32 row, i32 lane)
    { return out.member(out.buffer_element(out.binding_member(draw, 0), w.word(u32(row))), lane); };
    auto const as_int = [&](flat_expr_id x) { return w.unary("convert_uint_to_int", x); };
    auto const var_of = [&](cc::string_view name, type_id type, flat_expr_id value, stmt_list& into)
    {
        auto const local = add_temporary_var(out, name, type);
        into.push_back(out.var(local, value));
        return local;
    };

    // One thread a primitive of one instance: `t = instance * primitives + p`, whose slots start at `t * group_words`.
    auto body = stmt_list();
    auto const thread = out.add_local(local_kind::parameter, "thread_id", out.type_named("int3"));
    auto const t = w.bound("geometry_thread", w.unary("convert_int_to_uint", out.member(out.local(thread), 0)), body);
    auto const primitives = w.bound("geometry_primitives", draw_value(0, 1), body);
    auto const instance = w.bound("geometry_instance", w.binary("divide_uint", t, primitives), body);
    auto const p = w.bound("geometry_primitive",
                           w.binary("subtract_uint", t, w.binary("multiply_uint", instance, primitives)), body);
    auto const base = w.bound("mesh_base", w.binary("multiply_uint", t, w.word(u32(layout.group_words))), body);
    // the counts start at nothing, so a geometry stage that returns before it appends leaves its slots empty
    body.push_back(out.assign(w.at(base, 0), w.word(0)));
    body.push_back(out.assign(w.at(base, 1), w.word(0)));

    // Each corner: its vertex from the index buffer or the draw's range, then the vertex stage on it.
    auto results = cc::vector<flat_expr_id>();
    for (auto c = 0; c < corners; ++c)
    {
        out.from = from;
        out.inlined_through = {};
        auto const k
            = w.bound("geometry_corner",
                      w.binary("add_uint", w.binary("multiply_uint", p, w.word(u32(corners))), w.word(u32(c))), body);
        auto result = run_vertex_stage(out, w, v, vertex_input, k, instance, body);
        if (result.has_error())
            return cc::error(result.error());
        results.push_back(result.value());
    }

    // the geometry stage's parameters, from what the corners computed
    out.from = from;
    out.inlined_through = {};
    out.e.locals[index_of(primitive)].kind = local_kind::let;
    body.push_back(out.let(primitive, out.construct(primitive_type, results)));
    for (auto const& si : g.stage_inputs)
    {
        out.e.locals[index_of(si.local)].kind = local_kind::let;
        if (si.input != stage_input::primitive_id)
            return cc::error(cc::format("{} reads a stage input the polyfill does not carry yet", g.name));
        body.push_back(out.let(si.local, as_int(p)));
    }
    out.e.locals[index_of(stream)].kind = local_kind::temporary;

    // What the stage appends: vertices in order, cut into primitives where a strip holds enough of them.
    auto const appended = var_of("geometry_vertices", w.uint_type(), w.word(0), body);
    auto const made = var_of("geometry_primitives_made", w.uint_type(), w.word(0), body);
    auto const strip = var_of("geometry_strip", w.uint_type(), w.word(0), body);
    auto const replace = [&](flat_stmt_id sid, stmt_list& into) -> bool
    {
        auto const* const eval = out.e.at(sid).node.try_as<flat_eval>();
        if (eval == nullptr || !is_valid(eval->value))
            return false;
        auto const write = stream_write_of(m, out.e.at(eval->value));
        if (write < 0)
            return false;
        auto const& s = out.e.at(sid);
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        if (write == 1)
        {
            into.push_back(out.assign(out.local(strip), w.word(0)));
            return true;
        }
        auto const arguments = cc::vector<flat_expr_id>::create_copy_of(
            cc::span<flat_expr_id const>(out.e.at(out.e.at(eval->value).node.as<flat_call>().arguments)));
        auto const value = w.bound("geometry_appended", arguments[1], into);
        auto const n = out.local(appended);
        auto room = stmt_list();
        auto const at = w.bound("mesh_at",
                                w.binary("add_uint", base,
                                         w.binary("add_uint", w.word(u32(layout.first_vertex_word())),
                                                  w.binary("multiply_uint", n, w.word(u32(layout.vertex_words))))),
                                room);
        for (auto i = isize(0); i < leaves.size(); ++i)
            room.push_back(out.assign(w.at(at, u32(i)), w.encode(w.leaf_of(value, leaves[i]), leaves[i].kind)));

        // a strip's primitive `j` is its corners `j` on; an odd triangle swaps its first two, so every one winds alike
        auto cut = stmt_list();
        auto const indices_at
            = w.bound("mesh_indices_at",
                      w.binary("add_uint", base,
                               w.binary("add_uint", w.word(u32(layout.first_index_word())),
                                        w.binary("multiply_uint", out.local(made), w.word(u32(layout.corners))))),
                      cut);
        auto const corner = [&](u32 back) { return w.binary("subtract_uint", out.local(appended), w.word(back)); };
        if (layout.corners == 3)
        {
            auto const odd
                = w.bound("geometry_odd",
                          w.binary("equal_uint", w.binary("bit_and_uint", out.local(strip), w.word(1)), w.word(1)), cut);
            auto const select = [&](flat_expr_id a, flat_expr_id b)
            {
                flat_expr_id const arguments[] = {odd, a, b};
                return out.call("select", arguments);
            };
            auto const first = w.bound("geometry_first", select(corner(1), corner(2)), cut);
            auto const second = w.bound("geometry_second", select(corner(2), corner(1)), cut);
            cut.push_back(out.assign(w.at(indices_at, 0), first));
            cut.push_back(out.assign(w.at(indices_at, 1), second));
            cut.push_back(out.assign(w.at(indices_at, 2), out.local(appended)));
        }
        else
        {
            cut.push_back(out.assign(w.at(indices_at, 0), corner(1)));
            cut.push_back(out.assign(w.at(indices_at, 1), out.local(appended)));
        }
        cut.push_back(out.assign(out.local(made), w.binary("add_uint", out.local(made), w.word(1))));
        cut.push_back(out.assign(w.at(base, 1), out.local(made)));
        room.push_back(out.if_(out.and_(w.binary("greater_equal_uint", out.local(strip), w.word(u32(layout.corners - 1))),
                                        w.binary("less_uint", out.local(made), w.word(u32(layout.max_primitives)))),
                               cut));

        room.push_back(out.assign(out.local(appended), w.binary("add_uint", out.local(appended), w.word(1))));
        room.push_back(out.assign(w.at(base, 0), out.local(appended)));
        room.push_back(out.assign(out.local(strip), w.binary("add_uint", out.local(strip), w.word(1))));
        // what a stage appends past `max_vertices` is dropped, as the native stage drops it
        into.push_back(out.if_(w.binary("less_uint", n, w.word(u32(layout.max_vertices))), room));
        return true;
    };
    auto const rest = rewritten(out, g.body, replace);
    for (auto const s : out.e.at(rest))
        body.push_back(s);

    // the header of a compute entry point dispatched by threads, whose threads past the grid return (CHK-520)
    auto& e = out.e;
    e.entry_stage = stage::compute;
    e.input = type_id::none;
    e.result = type_id::none;
    e.stage_inputs = {{.input = stage_input::thread_id, .local = thread}};
    e.workgroup = {k_geometry_polyfill_workgroup, 1, 1};
    e.is_auto_workgroup = true;
    e.legalized = nullptr;
    merge_stage_into(e, v);
    e.features.remove(feature::geometry_shader);
    return compacted(m, e, body);
}

flat_entry_point sgl::check::geometry_as_vertex_fetch(checked_module const& m, flat_entry_point const& g)
{
    auto const layout = geometry_polyfill_layout_of(m, g);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    return slots_as_vertex_fetch(m, g, layout.value(), m.at(g.at(stream_of(m, g)).type).element);
}
