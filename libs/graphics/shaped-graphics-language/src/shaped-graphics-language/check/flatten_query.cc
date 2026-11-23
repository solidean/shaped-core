#include <shaped-graphics-language/check/impl/flattener.hh>
#include <shaped-graphics-language/check/impl/query.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Queries in the flat tree (EMIT-275 to EMIT-277).
//
// `let q = world.query(r, …)` writes its arguments alone, each evaluated once, and `q` stands for a position in
// `queries`; nothing of the target is started before the loop, which no statement between could tell.
// `for c in q.candidates:` is the trace's two forms, of which a target writes one (`flat_by_target`):
//
//     var hit
//     by_target(
//         native:   let q = ray_query_begin(); start the trace; block $exit { query_native_step(q, body) };
//                   hit = the committed hit
//         emulated: var s = query_walk_start(r); block $exit { query_emulated_<g>(world, …, mut s, body) };
//                   hit = what s holds
//     )
//
// `body` is the loop's body, written again in the frame the `for` stood in wherever the prelude's loop calls it.
// There a `break` leaves `$exit`, a `continue` leaves the candidate's own block, and a candidate's member is the
// prelude's `query_native_m` or `query_emulated_m` of what the form hands the body.
// In the native form every leave out of the body is preceded by the target's abort (EMIT-277).

namespace
{
/// The prelude's symbol `name`, its first overload; none where the prelude lacks it.
symbol_id prelude_symbol(checker const& c, cc::string_view name)
{
    auto const* const found = c.prelude_names.get_ptr(name);
    return found == nullptr || found->empty() ? symbol_id::none : found->front();
}

/// The bits of a ray flag of the prelude's `ray_flags`, by its case's name.
u32 flag_bit(checked_module const& m, type_id flags, cc::string_view name)
{
    for (auto const& k : m.at(m.at(flags).cases))
        if (k.name == name)
            return u32(k.value);
    return 0;
}
} // namespace

auto sgl::check::impl::flattener::query_of(flat_expr_id value) const -> i32
{
    if (!is_valid(value))
        return -1;
    auto const& x = entry.at(value);
    if (!is_valid(x.type) || x.type == checked_module::error_type || !is_query_kind(c.out.at(x.type).kind))
        return -1;
    auto const* const literal = x.node.try_as<flat_int_literal>();
    if (literal == nullptr || literal->value < 0 || literal->value >= queries.size())
        return -1;
    return literal->value;
}

auto sgl::check::impl::flattener::query_helper(ast::expr_id id,
                                               cc::string_view name,
                                               cc::span<flat_expr_id const> arguments,
                                               cc::span<i32 const> handed) -> flat_expr_id
{
    auto const callee = prelude_symbol(c, name);
    if (!is_valid(callee) || c.out.at(callee).info < 0)
        return fail();
    for (auto i = isize(0); i < arguments.size(); ++i)
        if (!is_valid(arguments[i]) && (i >= handed.size() || handed[i] < 0))
            return fail();
    auto slots = cc::vector<i32>();
    for (auto i = isize(0); i < arguments.size(); ++i)
        slots.push_back(i32(i));
    auto const inlined = inline_bound(id, callee, arguments, slots, handed);
    if (!is_valid(inlined.label))
        return fail();
    return add_expr(c.out.functions[c.out.at(callee).info].result, id,
                    flat_block{.label = inlined.label, .body = inlined.body});
}

void sgl::check::impl::flattener::flatten_query_let(origin from, ast::stmt_id id, ast::let_stmt const& let)
{
    auto const record_index = tables().call_at(let.value);
    auto const is_discarded = ast::is_valid(let.pattern) && ast().at(let.pattern).node.is<ast::wildcard>();
    auto const* const name = ast::is_valid(let.pattern) ? ast().at(let.pattern).node.try_as<ast::name>() : nullptr;
    if (record_index < 0 || (name == nullptr && !is_discarded))
    {
        mark_failed();
        return;
    }
    auto const& record = c.out.call_records[record_index];
    auto const& callee = c.out.at(record.callee);
    if (callee.info < 0)
    {
        mark_failed();
        return;
    }
    auto const parameters = c.out.at(c.out.functions[callee.info].parameters);
    auto const slots = cc::vector<i32>::create_copy_of(c.out.at(record.slots));
    auto const written = cc::vector<written_argument>::create_copy_of(c.out.at(record.written));
    auto const values = flatten_written(written);
    if (parameters.size() != 4 || slots.size() != 4)
    {
        mark_failed();
        return;
    }
    auto state = query_state{.geometry = c.out.at(tables().type_at(let.value)).format};
    // EVAL-80: each argument evaluated once, in the order written, where the `let` stands
    flat_expr_id given[4] = {flat_expr_id::none, flat_expr_id::none, flat_expr_id::none, flat_expr_id::none};
    for (auto k = isize(0); k < values.size(); ++k)
    {
        auto p = -1;
        for (auto i = 0; i < 4; ++i)
            if (slots[i] == k)
                p = i;
        if (p < 0 || !is_valid(values[k]))
        {
            mark_failed();
            return;
        }
        auto value = values[k];
        if (p == 0)
            value = resource_stand_in(value);
        else if (!is_substitutable(value))
        {
            auto const local = add_local(local_kind::let, parameters[p].name, entry.at(value).type);
            add_stmt(from, flat_let{.local = local, .value = value});
            value = local_ref(local, let.value);
        }
        given[p] = value;
    }
    if (!is_valid(given[0]) || !is_valid(given[1]))
    {
        mark_failed();
        return;
    }
    auto const flags_type = parameters[2].type;
    if (!is_valid(given[2]))
    {
        auto none_case = -1;
        auto const cases = c.out.at(c.out.at(flags_type).cases);
        for (auto i = isize(0); i < cases.size(); ++i)
            if (cases[i].name == "none")
                none_case = i32(i);
        given[2] = enum_value(flags_type, let.value, none_case);
    }
    if (!is_valid(given[3]))
        given[3] = add_expr(parameters[3].type, let.value, flat_int_literal{.value = 255});

    // CHK-540: the flags are a constant once the entry point is inlined, which decides what the loop meets
    auto const flags_value = slots[2] >= 0 ? values[slots[2]] : given[2];
    auto const is_flags_constant = is_constant(c.out, entry, flags_value);
    auto const evaluated = is_flags_constant ? evaluate_constant(c.out, entry, flags_value) : outcome{};
    if (!is_flags_constant || evaluated.status != run_status::ok || evaluated.result.leaves.size() != 1)
    {
        auto const at = slots[2] >= 0 ? written[slots[2]].expr : let.value;
        (void)refuse_query(diagnostic_kind::invalid_constant_argument, {.file = file(), .expr = at},
                           "a query's flags are a constant once the entry point is inlined, since they decide what its "
                           "loop meets; a trace takes flags that are none");
        return;
    }
    state.flag_bits = evaluated.result.leaves[0].bits;
    state.world = given[0];
    state.ray = given[1];
    state.flags = given[2];
    state.mask = given[3];
    // CHK-55: `let _ = world.query(r)` evaluates the arguments, and a query nobody steps through starts no trace
    if (is_discarded)
        return;
    auto const index = i32(queries.size());
    queries.push_back(state);
    auto const stand_in = add_expr(tables().type_at(let.pattern), let.value, flat_int_literal{.value = index});
    current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .literal = stand_in});
}

void sgl::check::impl::flattener::flatten_candidate_loop(origin from, ast::stmt_id id, ast::for_stmt const& loop)
{
    auto const& member = ast().at(loop.iterable).node.as<ast::member>();
    auto const object = flatten_expr(member.object);
    auto const q = query_of(object);
    // the check pass held the loop to its query's `let`, so a loop that meets another value met one handed on
    if (q < 0 || queries[q].query >= 0)
    {
        if (is_valid(object) && is_query_kind(c.out.at(entry.at(object).type).kind))
            (void)refuse_query(diagnostic_kind::query_escapes, {.file = file(), .expr = member.object},
                               "a query is the `let` of its `world.query`, and what hands one on holds it");
        else
            mark_failed();
        return;
    }
    if (queries[q].is_stepped || queries[q].is_stepping)
    {
        (void)refuse_query(diagnostic_kind::query_traversal_misplaced, {.file = file(), .expr = loop.iterable},
                           "a query is stepped through once, by one loop, and this query has one already");
        return;
    }
    auto const where = loop.iterable;
    auto const geometry = queries[q].geometry;
    auto const geometry_name = cc::string(k_geometry_kinds[geometry]);
    auto const hit_helper = prelude_symbol(c, cc::format("query_native_hit_{}", geometry_name));
    if (!is_valid(hit_helper) || c.out.at(hit_helper).info < 0)
    {
        mark_failed();
        return;
    }
    auto const hit_type = c.out.functions[c.out.at(hit_helper).info].result;
    auto const flags_type = entry.at(queries[q].flags).type;
    auto const bits = queries[q].flag_bits;
    auto const skips_triangles = (bits & flag_bit(c.out, flags_type, "skip_triangles")) != 0;
    auto const skips_boxes = (bits & flag_bit(c.out, flags_type, "skip_procedural")) != 0;
    auto const ends = (bits & flag_bit(c.out, flags_type, "accept_first_hit_and_end_search")) != 0;
    // CHK-540: a query of no kind its structure holds meets nothing, which a mask of 0 asks of every form
    auto const meets_nothing = (geometry == 0 && skips_triangles) || (geometry == 1 && skips_boxes)
                            || (geometry == 2 && skips_triangles && skips_boxes);
    // the start of a mixed query drops the facing culls beside `skip_triangles`, which the target forbids together
    auto const start = cc::format("query_native_start_{}", geometry_name);

    auto const hit = add_local(local_kind::var, "hit", hit_type);
    queries[q].hit = hit;
    add_stmt(from, flat_var{.local = hit});
    queries[q].hit_writes.push_back(block.back());

    // the body is written again in the frame the `for` stood in, wherever a form's loop calls it
    auto const loop_index = i32(candidate_loops.size());
    auto around = frames.back();
    around.stands_for_root = is_root_frame();
    candidate_loops.push_back({.query = q, .file = file(), .loop = id, .around = cc::move(around)});
    auto const closure_index = i32(closures.size());
    closures.push_back({.file = file(), .owner = current()->function, .candidate_loop = loop_index});
    queries[q].is_stepping = true;

    auto const mask = [&]() -> flat_expr_id
    {
        return meets_nothing ? add_expr(entry.at(queries[q].mask).type, where, flat_int_literal{.value = 0})
                             : again(queries[q].mask, where);
    };
    // A void helper's block as a statement of its own.
    auto const run = [&](cc::string_view name, cc::span<flat_expr_id const> arguments, cc::span<i32 const> handed)
    {
        auto const value = query_helper(where, name, arguments, handed);
        if (!is_valid(value))
            return;
        auto const b = entry.at(value).node.as<flat_block>();
        add_stmt(from, flat_block{.label = b.label, .body = b.body});
    };
    auto const form = [&](bool is_native) -> flat_expr_id
    {
        auto const outer = cc::move(block);
        block = {};
        auto const label = add_label(is_native ? "native" : "emulated");
        auto const exit = add_label("candidates");
        candidate_loops[loop_index].exit = exit;
        candidate_loops[loop_index].is_native = is_native;
        auto committed = flat_expr_id::none;
        if (is_native)
        {
            auto const begin = prelude_symbol(c, "ray_query_begin");
            if (!is_valid(begin) || c.out.at(begin).info < 0)
            {
                block = cc::move(outer);
                return fail();
            }
            auto const handle = add_local(local_kind::let, "q", c.out.functions[c.out.at(begin).info].result);
            add_stmt(from, flat_let{.local = handle, .value = builtin_call(where, begin, {})});
            candidate_loops[loop_index].handle = handle;
            candidate_loops[loop_index].ended = local_id::none;
            flat_expr_id const start_arguments[] = {local_ref(handle, where), again(queries[q].world, where),
                                                    again(queries[q].ray, where), again(queries[q].flags, where), mask()};
            run(start, start_arguments, {});
            // EMIT-277: a commit that ends the search is an exit of its own, taken once the candidate's body is done
            if (ends)
            {
                auto const ended = add_local(local_kind::temporary, "ended", bool_type());
                add_stmt(from, flat_var{.local = ended, .value = add_expr(bool_type(), where, flat_bool_literal{})});
                candidate_loops[loop_index].ended = ended;
            }
            auto const steps = cc::move(block);
            block = {};
            flat_expr_id const step_arguments[] = {local_ref(handle, where), flat_expr_id::none};
            i32 const handed[] = {-1, closure_index};
            run("query_native_step", step_arguments, handed);
            auto const stepped = add_list(block);
            block = steps;
            add_stmt(from, flat_block{.label = exit, .body = stepped});
            flat_expr_id const committed_arguments[] = {local_ref(handle, where), again(queries[q].ray, where)};
            committed = query_helper(where, cc::format("query_native_hit_{}", geometry_name), committed_arguments);
        }
        else
        {
            auto const walk_start = prelude_symbol(c, "query_walk_start");
            if (!is_valid(walk_start) || c.out.at(walk_start).info < 0)
            {
                block = cc::move(outer);
                return fail();
            }
            auto const walk_type = c.out.functions[c.out.at(walk_start).info].result;
            auto const walk = add_local(local_kind::var, "walk", walk_type);
            flat_expr_id const start_arguments[] = {again(queries[q].ray, where)};
            add_stmt(from, flat_var{.local = walk, .value = query_helper(where, "query_walk_start", start_arguments)});
            candidate_loops[loop_index].walk = walk;
            auto const before = cc::move(block);
            block = {};
            flat_expr_id const step_arguments[] = {again(queries[q].world, where), again(queries[q].ray, where),
                                                   again(queries[q].flags, where), mask(),
                                                   local_ref(walk, where),         flat_expr_id::none};
            i32 const handed[] = {-1, -1, -1, -1, -1, closure_index};
            run(cc::format("query_emulated_{}", geometry_name), step_arguments, handed);
            auto const stepped = add_list(block);
            block = before;
            add_stmt(from, flat_block{.label = exit, .body = stepped});
            flat_expr_id const committed_arguments[] = {local_ref(walk, where), again(queries[q].ray, where)};
            committed = query_helper(where, cc::format("query_emulated_hit_{}", geometry_name), committed_arguments);
        }
        add_stmt(from, flat_assign{.place = local_ref(hit, where), .value = committed});
        queries[q].hit_writes.push_back(block.back());
        add_stmt(from, flat_leave{.target = label});
        auto const body = add_list(block);
        block = cc::move(outer);
        return add_expr(checked_module::void_type, where, flat_block{.label = label, .body = body});
    };
    auto const native = form(true);
    auto const emulated = form(false);
    queries[q].is_stepping = false;
    queries[q].is_stepped = true;
    if (!is_valid(native) || !is_valid(emulated))
    {
        mark_failed();
        return;
    }
    add_stmt(from, flat_eval{.value = add_expr(checked_module::void_type, where,
                                               flat_by_target{.native = native, .emulated = emulated})});
}

auto sgl::check::impl::flattener::call_candidate_body(ast::expr_id id, i32 loop, cc::span<flat_expr_id const> values)
    -> flat_expr_id
{
    // by value: writing the body may open loops of its own, and the vector moves
    auto const l = candidate_loops[loop];
    if (values.empty() || !is_valid(values[0]))
        return fail();
    auto const where = origin{.file = file(), .expr = id};
    auto handle = local_id::none;
    if (auto const* const ref = entry.at(values[0]).node.try_as<flat_local_ref>();
        ref != nullptr && is_substitutable(values[0]))
        handle = ref->local;
    else
    {
        handle = add_local(local_kind::let, "candidate", entry.at(values[0]).type);
        add_stmt(where, flat_let{.local = handle, .value = values[0]});
    }
    auto const& for_ast = c.ast_of(l.file);
    auto const& for_stmt = for_ast.at(l.loop).node.as<ast::for_stmt>();
    auto const tables_of_loop = table_view{.module = &c.out, .file = l.file, .instance = l.around.instance};
    auto const candidate_type = tables_of_loop.type_at(for_stmt.variable);
    auto const index = i32(queries.size());
    queries.push_back({.query = l.query,
                       .geometry = queries[l.query].geometry,
                       .is_native = l.is_native,
                       .handle = handle,
                       .walk = l.walk,
                       .ended = l.ended});

    auto const outer = cc::move(block);
    block = {};
    auto const next = add_label("candidate");
    auto body_frame = l.around;
    body_frame.loops.push_back({.loop = l.exit, .continue_block = next});
    body_frame.is_candidate_body = true;
    frames.push_back(cc::move(body_frame));
    auto const stand_in = add_expr(candidate_type, for_stmt.variable, flat_int_literal{.value = index});
    current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(l.loop)}, .literal = stand_in});
    // CHK-543: the body is a decision, which the driver runs as often as it meets candidates
    auto const outer_decision = inlined_decision;
    auto const outer_candidate = is_candidate_body;
    inlined_decision = stage::any_hit;
    is_candidate_body = true;
    if (l.is_native)
        open_native_loops.push_back(loop);
    auto const statements = cc::move(block);
    block = {};
    for (auto const stmt : for_ast.at(for_stmt.body.statements))
        flatten_stmt(stmt);
    auto const candidate = add_list(block);
    block = statements;
    add_stmt({.file = l.file, .stmt = l.loop}, flat_block{.label = next, .body = candidate});
    // EMIT-277: a commit under `accept_first_hit_and_end_search` ends the traversal once the candidate's body is done
    if (l.is_native && is_valid(l.ended))
    {
        auto const abort = prelude_symbol(c, "ray_query_abort");
        flat_expr_id const handle_argument[] = {local_ref(l.handle, for_stmt.iterable)};
        flat_stmt_id const ended[] = {
            make_stmt({.file = l.file, .stmt = l.loop},
                      flat_eval{.value = builtin_call(for_stmt.iterable, abort, handle_argument)}),
            make_stmt({.file = l.file, .stmt = l.loop}, flat_leave{.target = l.exit}),
        };
        add_stmt({.file = l.file, .stmt = l.loop},
                 flat_if{.condition = local_ref(l.ended, for_stmt.iterable), .then_body = add_list(ended)});
    }
    if (l.is_native)
        open_native_loops.remove_back();
    inlined_decision = outer_decision;
    is_candidate_body = outer_candidate;
    frames.remove_back();
    auto const body = add_list(block);
    block = cc::move(outer);
    return add_expr(checked_module::void_type, id, flat_block{.label = add_label("body"), .body = body});
}

void sgl::check::impl::flattener::abort_crossed(origin from, label_id target, flat_expr_id* value)
{
    auto const crosses
        = [&](i32 loop) { return !is_valid(target) || index_of(target) <= index_of(candidate_loops[loop].exit); };
    auto is_any = false;
    for (auto const loop : open_native_loops)
        is_any = is_any || crosses(loop);
    if (!is_any)
        return;
    // the value is read before the abort, since what it reads of the candidate is gone after it
    if (value != nullptr && is_valid(*value) && !is_substitutable(*value))
    {
        auto const kept = add_local(local_kind::temporary, "left", entry.at(*value).type);
        add_stmt(from, flat_let{.local = kept, .value = *value});
        *value = local_ref(kept, from.expr);
    }
    for (auto i = open_native_loops.size() - 1; i >= 0; --i)
    {
        auto const& l = candidate_loops[open_native_loops[i]];
        if (is_valid(target) && index_of(target) > index_of(l.exit))
            continue;
        auto const abort = prelude_symbol(c, "ray_query_abort");
        auto const at = ast::is_valid(from.expr) ? from.expr : ast::expr_id::none;
        flat_expr_id const handle[] = {local_ref(l.handle, at)};
        add_stmt(from, flat_eval{.value = builtin_call(at, abort, handle)});
    }
}

auto sgl::check::impl::flattener::flatten_query_member(ast::expr_id id, target const& where) -> flat_expr_id
{
    auto const& node = ast().at(id).node;
    auto object = ast::expr_id::none;
    auto arguments = cc::span<ast::argument const>();
    if (auto const* const m = node.try_as<ast::member>())
        object = m->object;
    else if (auto const* const call = node.try_as<ast::call>())
    {
        object = ast().at(call->callee).node.as<ast::member>().object;
        arguments = ast().at(call->arguments);
    }
    auto const value = flatten_expr(object);
    auto const q = query_of(value);
    if (q < 0 || where.index < 0)
    {
        // CHK-541: a member of what a call, an `if` or a field handed on, which held the query to hand it
        if (is_valid(value) && is_query_kind(c.out.at(entry.at(value).type).kind))
            return refuse_query(diagnostic_kind::query_escapes, {.file = file(), .expr = object},
                                "a query is the `let` of its `world.query` and a candidate the variable of its loop, "
                                "and what hands one on holds it");
        return fail();
    }
    auto const member = query_member(where.index);
    if (member == query_member::hit)
    {
        // CHK-546: after the query's loop alone, which a query handed to a function shows only once inlined
        auto const& query = queries[q];
        if (query.query >= 0)
            return fail();
        if (!query.is_stepped || query.is_stepping)
            return refuse_query(diagnostic_kind::query_hit_too_early, {.file = file(), .expr = id},
                                query.is_stepping ? cc::string("a query's hit is read after its candidate loop, and "
                                                               "this read stands inside it")
                                                  : cc::string("a query's hit is read after its candidate loop, and "
                                                               "this query has none before here"));
        queries[q].is_hit_read = true;
        return local_ref(query.hit, id);
    }
    auto const candidate = queries[q];
    if (candidate.query < 0)
        return fail();
    auto const& query = queries[candidate.query];
    auto const is_mixed = query.geometry == 2;
    auto const name = k_query_members[where.index].name;
    // on a structure of one kind, every candidate is of that kind
    if ((member == query_member::is_triangle || member == query_member::is_procedural) && !is_mixed)
        return add_expr(bool_type(), id,
                        flat_bool_literal{.value = (query.geometry == 0) == (member == query_member::is_triangle)});
    auto const ray = again(query.ray, id);
    auto const flags_type = entry.at(query.flags).type;
    auto const ends = (query.flag_bits & flag_bit(c.out, flags_type, "accept_first_hit_and_end_search")) != 0;
    if (member == query_member::commit || member == query_member::commit_at)
    {
        auto const t = member == query_member::commit_at && !arguments.empty() ? flatten_expr(arguments[0].value)
                                                                               : flat_expr_id::none;
        auto const mixed = add_expr(bool_type(), id, flat_bool_literal{.value = is_mixed});
        auto committed = flat_expr_id::none;
        if (candidate.is_native)
        {
            auto const handle = local_ref(candidate.handle, id);
            if (member == query_member::commit)
            {
                flat_expr_id const commit_arguments[] = {handle, ray, mixed};
                committed = query_helper(id, "query_native_commit", commit_arguments);
            }
            else
            {
                flat_expr_id const commit_arguments[] = {handle, ray, t, mixed};
                committed = query_helper(id, "query_native_commit_at", commit_arguments);
            }
            // EMIT-277: a commit that ends the search marks it, inside the commit's own block, so it stays as conditional as
            // the commit; the candidate's body ends the traversal after it
            if (is_valid(committed) && is_valid(candidate.ended))
            {
                auto const at = origin{.file = file(), .expr = id};
                auto const label = add_label("committed");
                auto const kept = add_local(local_kind::temporary, "committed", bool_type());
                flat_stmt_id const mark[]
                    = {make_stmt(at, flat_assign{.place = local_ref(candidate.ended, id),
                                                 .value = add_expr(bool_type(), id, flat_bool_literal{.value = true})})};
                flat_stmt_id const body[] = {
                    make_stmt(at, flat_let{.local = kept, .value = committed}),
                    make_stmt(at, flat_if{.condition = local_ref(kept, id), .then_body = add_list(mark)}),
                    make_stmt(at, flat_leave{.target = label, .value = local_ref(kept, id)}),
                };
                committed = add_expr(bool_type(), id, flat_block{.label = label, .body = add_list(body)});
            }
            return committed;
        }
        auto const ended = add_expr(bool_type(), id, flat_bool_literal{.value = ends});
        auto const walk = local_ref(candidate.walk, id);
        auto const handle = local_ref(candidate.handle, id);
        if (member == query_member::commit)
        {
            flat_expr_id const commit_arguments[] = {walk, handle, ray, mixed, ended};
            return query_helper(id, "query_emulated_commit", commit_arguments);
        }
        flat_expr_id const commit_arguments[] = {walk, handle, ray, t, mixed, ended};
        return query_helper(id, "query_emulated_commit_at", commit_arguments);
    }
    if (candidate.is_native)
    {
        flat_expr_id const read_arguments[] = {local_ref(candidate.handle, id), ray};
        return query_helper(id, cc::format("query_native_{}", name), read_arguments);
    }
    flat_expr_id const read_arguments[] = {local_ref(candidate.handle, id), local_ref(candidate.walk, id), ray};
    return query_helper(id, cc::format("query_emulated_{}", name), read_arguments);
}

namespace
{
/// The first node of a tree reachable from `body` whose type holds a query or a candidate (CHK-541).
struct query_escape_walk
{
    checked_module const& m;
    flat_entry_point const& e;
    cc::vector<bool> is_seen;
    cc::optional<origin> found;

    [[nodiscard]] bool holds(type_id type, int depth) const
    {
        if (!is_valid(type) || type == checked_module::error_type || depth > k_max_depth)
            return false;
        auto const& t = m.at(type);
        if (is_query_kind(t.kind))
            return true;
        if (t.kind == type_kind::array)
            return holds(t.element, depth + 1);
        if (t.kind != type_kind::structure || m.builtin_type_of(type) != nullptr)
            return false;
        for (auto const& member : m.at(t.members))
            if (member.type != type && holds(member.type, depth + 1))
                return true;
        return false;
    }

    void expr(flat_expr_id id, int depth)
    {
        if (found.has_value() || !is_known(e, id) || depth > k_max_depth || is_seen[index_of(id)])
            return;
        is_seen[index_of(id)] = true;
        auto const& x = e.at(id);
        if (holds(x.type, 0))
        {
            found = x.from;
            return;
        }
        if (auto const* const b = x.node.try_as<flat_block>())
            return body(b->body, depth + 1);
        if (auto const* const both = x.node.try_as<flat_by_target>())
        {
            expr(both->native, depth + 1);
            expr(both->emulated, depth + 1);
            return;
        }
        for_each_operand(e, x, [&](flat_expr_id operand) { expr(operand, depth + 1); });
    }

    void stmt(flat_stmt_id id, int depth)
    {
        if (found.has_value() || !is_known(e, id) || depth > k_max_depth)
            return;
        auto const& s = e.at(id);
        auto local = local_id::none;
        if (auto const* const let = s.node.try_as<flat_let>())
            local = let->local;
        else if (auto const* const var = s.node.try_as<flat_var>())
            local = var->local;
        if (is_known(e, local) && holds(e.at(local).type, 0))
        {
            found = s.from;
            return;
        }
        for_each_expr_of(e, s, [&](flat_expr_id x) { expr(x, depth + 1); });
        for_each_pattern_of(e, s,
                            [&](ast::range_of<flat_expr_id> patterns)
                            {
                                if (is_known(e, patterns))
                                    for (auto const p : e.at(patterns))
                                        expr(p, depth + 1);
                            });
        for_each_body_of(e, s, [&](ast::range_of<flat_stmt_id> b) { body(b, depth + 1); });
    }

    void body(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (!is_known(e, range))
            return;
        for (auto const s : e.at(range))
            stmt(s, depth);
    }
};
} // namespace

void sgl::check::impl::flattener::judge_queries()
{
    if (is_query_refused)
        return;
    // a hit nothing reads is never written, so a target reads nothing of what the traversal committed
    for (auto const& q : queries)
        if (q.query < 0 && !q.is_hit_read)
            for (auto const s : q.hit_writes)
                entry.stmts[index_of(s)].node = flat_block{.label = add_label("unread")};
    auto walk
        = query_escape_walk{.m = c.out, .e = entry, .is_seen = cc::vector<bool>::create_filled(entry.exprs.size(), false)};
    for (auto const s : hoisted_lets)
        walk.stmt(s, 0);
    for (auto const s : block)
        walk.stmt(s, 0);
    if (!walk.found.has_value())
        return;
    (void)refuse_query(diagnostic_kind::query_escapes, walk.found.value(),
                       "a query is the `let` of its `world.query` and a candidate the variable of its loop, which a "
                       "function may be handed; no other local, field, element, `if` or `return` holds one");
}

auto sgl::check::impl::flattener::refuse_query(diagnostic_kind kind, origin at, cc::string detail) -> flat_expr_id
{
    auto const where = ast::is_valid(at.expr) ? c.span_of(at.file, at.expr) : c.span_of(at.file, at.stmt);
    unroll_reports.push_back(
        {.kind = kind, .file = at.file, .where = where, .detail = cc::move(detail), .site = innermost_call()});
    // reported, so the tree's failure needs no `unsupported-yet` of its own, and no escape the scan would find again
    meets_error = true;
    is_query_refused = true;
    return fail();
}
