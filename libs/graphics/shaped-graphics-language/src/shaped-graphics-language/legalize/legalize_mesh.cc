#include <clean-core/common/assertf.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-524: a mesh stage on a target without one is a compute pass and a draw.
// The compute form writes each workgroup's counts, indices and vertices into fixed slots of `sg_mesh_output`, and the
// vertex form reads them back from `sg_mesh_input`, one invocation a corner of every primitive slot in order, so the
// draw keeps the primitive order the native stage has.

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

} // namespace

cc::result<mesh_polyfill_layout, cc::string> sgl::check::mesh_polyfill_layout_of(checked_module const& m,
                                                                                 flat_entry_point const& e)
{
    CC_ASSERT(e.entry_stage == stage::mesh, "only a mesh entry point has a polyfill layout");
    auto const output = output_of(m, e);
    if (!is_valid(output))
        return cc::error(cc::format("{} takes no mesh output", e.name));
    auto const& t = m.at(e.at(output).type);
    if (is_valid(t.primitive))
        return cc::error(cc::format("{} writes values per primitive, which the polyfill does not carry yet", e.name));
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto is_input = false;
        for (auto const& input : e.stage_inputs)
            is_input = is_input || index_of(input.local) == i;
        if (e.locals[i].kind == local_kind::parameter && local_id(i) != output && !is_input)
            return cc::error(cc::format("{} takes a task stage's payload, which the polyfill does not carry yet", e.name));
    }
    auto const leaves = leaves_of(m, t.element);
    if (leaves.has_error())
        return cc::error(cc::format("{}'s vertex holds a '{}', which no word holds", e.name, leaves.error()));

    auto const& info = m.functions[m.at(e.function).info];
    auto layout = mesh_polyfill_layout{.corners = info.is_line_mesh ? 2 : 3,
                                       .max_vertices = info.max_vertices,
                                       .max_primitives = info.max_primitives,
                                       .vertex_words = i32(leaves.value().size())};
    layout.group_words = layout.first_vertex_word() + layout.max_vertices * layout.vertex_words;
    return layout;
}

flat_entry_point sgl::check::mesh_as_compute(checked_module const& m, flat_entry_point const& e)
{
    auto const layout = mesh_polyfill_layout_of(m, e);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    auto const output = output_of(m, e);
    auto const vertex_type = m.at(e.at(output).type).element;
    auto const leaves = leaves_of(m, vertex_type).value();

    auto out = flat_builder::extend(m, e);
    auto w = words{.out = out, .binding = listed(out, "sg_mesh_output")};
    auto const grid = listed(out, "sg_dispatch_grid");

    auto group = local_id::none;
    for (auto const& input : out.e.stage_inputs)
        if (input.input == stage_input::workgroup_id)
            group = input.local;
    if (!is_valid(group))
    {
        group = out.add_local(local_kind::parameter, "workgroup_id", out.type_named("int3"));
        out.e.stage_inputs.push_back({.input = stage_input::workgroup_id, .local = group});
    }

    // `base = (x + nx * (y + ny * z)) * group_words`, the grid being the threads sg dispatched
    auto prologue = stmt_list();
    auto const lane = [&](flat_expr_id v, i32 axis) { return out.member(v, axis); };
    auto const id = [&](i32 axis) { return w.unary("convert_int_to_uint", lane(out.local(group), axis)); };
    auto const threads = out.binding_member(grid, 0);
    auto const nx = w.bound("mesh_nx", w.binary("divide_uint", lane(threads, 0), w.word(u32(e.workgroup[0]))), prologue);
    auto const ny = w.bound(
        "mesh_ny", w.binary("divide_uint", lane(out.binding_member(grid, 0), 1), w.word(u32(e.workgroup[1]))), prologue);
    auto const linear
        = w.binary("add_uint", id(0),
                   w.binary("multiply_uint", nx, w.binary("add_uint", id(1), w.binary("multiply_uint", ny, id(2)))));
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
                store(first, c, lane(indices, i32(c)));
            return true;
        }
        // a vertex: its leaves in field order
        auto const value = w.bound("mesh_vertex", arguments[2], into);
        auto const first = w.bound(
            "mesh_at",
            w.binary("add_uint", base,
                     w.binary("add_uint", w.word(u32(layout.value().first_vertex_word())),
                              w.binary("multiply_uint", arguments[1], w.word(u32(layout.value().vertex_words))))),
            into);
        for (auto k = isize(0); k < leaves.size(); ++k)
            store(first, u32(k), w.encode(w.leaf_of(value, leaves[k]), leaves[k].kind));
        return true;
    };

    auto body = stmt_list::create_copy_of(cc::span<flat_stmt_id const>(prologue));
    auto const rest = rewritten(out, out.e.body, replace);
    for (auto const s : out.e.at(rest))
        body.push_back(s);
    out.e.locals[index_of(output)].kind = local_kind::temporary;
    // an output that is the first parameter stands as the entry point's stage struct, which a compute stage has none of
    if (out.e.input == out.e.at(output).type)
        out.e.input = type_id::none;
    out.e.entry_stage = stage::compute;
    out.e.features.remove(feature::mesh_shader);
    // the slots' base and the grid are read by every store, which the compaction gives a node each
    return compacted(m, out.e, body);
}

flat_entry_point sgl::check::mesh_as_vertex_fetch(checked_module const& m, flat_entry_point const& e)
{
    auto const layout = mesh_polyfill_layout_of(m, e);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    return slots_as_vertex_fetch(m, e, layout.value(), m.at(e.at(output_of(m, e)).type).element);
}
