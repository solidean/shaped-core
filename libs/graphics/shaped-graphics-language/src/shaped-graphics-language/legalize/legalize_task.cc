#include <clean-core/common/assertf.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-524: a task stage on a target without one is a compute pass that writes each workgroup's launch into a record,
// a scan that numbers the launched mesh workgroups in order, and the mesh stage's compute half over exactly those.
// The scan keeps the native order, task workgroup by task workgroup and then by mesh workgroup, which an append in
// completion order would lose.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// The payload `dispatch_mesh` hands on, the struct the task stage's function states (CHK-485).
[[nodiscard]] type_id payload_of(checked_module const& m, flat_entry_point const& t)
{
    return m.functions[m.at(t.function).info].mesh_payload;
}

[[nodiscard]] bool is_dispatch_mesh(checked_module const& m, flat_expr const& x)
{
    auto const* const call = x.node.try_as<flat_call>();
    auto const* const record = call == nullptr ? nullptr : m.builtin_function(call->intrinsic);
    return record != nullptr && record->name == "dispatch_mesh";
}
} // namespace

cc::result<task_polyfill_layout, cc::string> sgl::check::task_polyfill_layout_of(checked_module const& m,
                                                                                 flat_entry_point const& t)
{
    CC_ASSERT(t.entry_stage == stage::task, "only a task entry point has a task polyfill layout");
    auto layout = task_polyfill_layout{.max_meshes = m.functions[m.at(t.function).info].max_meshes};
    if (auto const payload = payload_of(m, t); is_valid(payload))
    {
        auto const leaves = leaves_of(m, payload);
        if (leaves.has_error())
            return cc::error(cc::format("{}'s payload holds a '{}', which no word holds", t.name, leaves.error()));
        layout.payload_words = i32(leaves.value().size());
    }
    return layout;
}

flat_entry_point sgl::check::task_as_compute(checked_module const& m, flat_entry_point const& t)
{
    using l = task_polyfill_layout;
    auto const layout = task_polyfill_layout_of(m, t);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());

    auto out = flat_builder::extend(m, t);
    auto w = words{.out = out, .binding = listed(out, "sg_task_output")};
    auto const grid = listed(out, "sg_dispatch_grid");
    auto const group = stage_input_of(out, stage_input::workgroup_id, "workgroup_id", out.type_named("int3"));
    auto const local
        = stage_input_of(out, stage_input::local_thread_index, "local_thread_index", out.type_named(builtins::k_int));

    // `x + nx * (y + ny * z)` over the header's grid, which an indirect draw's arguments wrote
    auto prologue = stmt_list();
    auto const nx = w.bound("task_nx", w.at(w.word(u32(l::k_task_grid_word))), prologue);
    auto const ny = w.bound("task_ny", w.at(w.word(u32(l::k_task_grid_word + 1))), prologue);
    auto const id = [&](i32 axis) { return w.unary("convert_int_to_uint", out.member(out.local(group), axis)); };
    auto const index = w.bound(
        "task_index",
        w.binary("add_uint", id(0),
                 w.binary("multiply_uint", nx, w.binary("add_uint", id(1), w.binary("multiply_uint", ny, id(2))))),
        prologue);
    auto const records = w.bound("task_records", out.member(out.binding_member(grid, 0), 0), prologue);

    // the first invocation's launch is the workgroup's, as on metal
    auto const replace = [&](flat_stmt_id sid, stmt_list& into) -> bool
    {
        auto const* const eval = out.e.at(sid).node.try_as<flat_eval>();
        if (eval == nullptr || !is_valid(eval->value) || !is_dispatch_mesh(m, out.e.at(eval->value)))
            return false;
        auto const& s = out.e.at(sid);
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto const arguments = cc::vector<flat_expr_id>::create_copy_of(
            cc::span<flat_expr_id const>(out.e.at(out.e.at(eval->value).node.as<flat_call>().arguments)));
        auto launch = stmt_list();
        auto const store = [&](flat_expr_id first, u32 offset, flat_expr_id value)
        { launch.push_back(out.assign(w.at(first, offset), value)); };
        auto const record
            = w.bound("task_record",
                      w.binary("add_uint", w.word(u32(l::k_first_record_word)),
                               w.binary("multiply_uint", index, w.word(u32(layout.value().record_words())))),
                      launch);
        for (auto axis = 0; axis < 3; ++axis)
            store(record, u32(l::k_record_grid + axis), arguments[axis]);
        if (arguments.size() > 3)
        {
            auto const payload = w.bound("task_payload", arguments[3], launch);
            auto const leaves = leaves_of(m, out.e.at(arguments[3]).type).value();
            for (auto k = isize(0); k < leaves.size(); ++k)
                store(record, u32(l::k_record_payload + k), w.encode(w.leaf_of(payload, leaves[k]), leaves[k].kind));
        }
        // a workgroup past the records the draw holds launches nothing, which the scan's count of them says too
        flat_expr_id const first[] = {out.local(local), out.int_literal(0)};
        flat_stmt_id const held[] = {out.if_(w.binary("less_uint", index, records), launch)};
        into.push_back(out.if_(out.call("equal_int", first), held));
        return true;
    };

    auto body = stmt_list::create_copy_of(cc::span<flat_stmt_id const>(prologue));
    auto const rest = rewritten(out, out.e.body, replace);
    for (auto const s : out.e.at(rest))
        body.push_back(s);
    out.e.input = type_id::none;
    out.e.entry_stage = stage::compute;
    out.e.features.remove(feature::mesh_shader);
    return compacted(m, out.e, body);
}

flat_entry_point sgl::check::mesh_task_scan(checked_module const& m,
                                            flat_entry_point const& e,
                                            task_polyfill_layout const& task)
{
    using l = task_polyfill_layout;
    auto const mesh = mesh_polyfill_layout_of(m, e, true);
    CC_ASSERTF(mesh.has_value(), "{}", mesh.has_error() ? mesh.error() : cc::string());

    auto out = fresh_entry(m, e, stage::compute);
    auto w = words{.out = out, .binding = listed(out, "sg_task_output")};
    auto const grid = listed(out, "sg_dispatch_grid");
    auto const uint = w.uint_type();
    auto const held = [&](i32 axis) { return out.member(out.binding_member(grid, 0), axis); };
    auto const product = [&](flat_expr_id first, u32 offset)
    {
        return w.binary("multiply_uint", w.at(first, offset),
                        w.binary("multiply_uint", w.at(first, offset + 1), w.at(first, offset + 2)));
    };
    auto const min = [&](flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const both[] = {a, b};
        return out.call("min", both);
    };

    // the task workgroups that wrote a record, which the launched mesh stage searches
    auto body = stmt_list();
    auto const tasks = w.bound("scan_tasks", min(product(w.word(0), u32(l::k_task_grid_word)), held(1)), body);
    body.push_back(out.assign(w.at(w.word(u32(l::k_task_count_word))), tasks));
    auto const running = out.add_local(local_kind::temporary, "scan_running", uint);
    auto const i = out.add_local(local_kind::temporary, "scan_task", uint);
    body.push_back(out.var(running, w.word(0)));
    body.push_back(out.var(i, w.word(0)));

    auto step = stmt_list();
    auto const record = w.bound("scan_record",
                                w.binary("add_uint", w.word(u32(l::k_first_record_word)),
                                         w.binary("multiply_uint", out.local(i), w.word(u32(task.record_words())))),
                                step);
    auto launched = product(record, u32(l::k_record_grid));
    // a launch past the stated bound is cut to it, so the draw's slots hold it
    if (task.max_meshes > 0)
        launched = min(launched, w.word(u32(task.max_meshes)));
    auto const counted = w.bound("scan_launched", launched, step);
    step.push_back(out.assign(w.at(record, u32(l::k_record_first)), out.local(running)));
    step.push_back(out.assign(out.local(running), w.binary("add_uint", out.local(running), counted)));
    step.push_back(out.assign(out.local(i), w.binary("add_uint", out.local(i), w.word(1))));
    body.push_back(out.while_(out.add_label("scan"), w.binary("less_uint", out.local(i), tasks), step));

    // the mesh workgroups sg holds slots for, which is where the launch is cut
    auto const total = w.bound("scan_total", min(out.local(running), held(0)), body);
    auto const vertices = u32(mesh.value().max_primitives * mesh.value().corners);
    auto const store = [&](i32 word, flat_expr_id value) { body.push_back(out.assign(w.at(w.word(u32(word))), value)); };
    store(l::k_launched_word, total);
    store(l::k_dispatch_word, total);
    store(l::k_dispatch_word + 1, w.word(1));
    store(l::k_dispatch_word + 2, w.word(1));
    store(l::k_draw_word, w.binary("multiply_uint", total, w.word(vertices)));
    store(l::k_draw_word + 1, w.word(1));
    store(l::k_draw_word + 2, w.word(0));
    store(l::k_draw_word + 3, w.word(0));
    return compacted(m, out.e, body);
}
