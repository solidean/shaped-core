#include <shaped-graphics-language/check/impl/flattener.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

auto sgl::check::impl::flattener::flatten_body(ast::body const& body) -> ast::range_of<flat_stmt_id>
{
    auto const outer = cc::move(block);
    block = {};
    for (auto const stmt : ast().at(body.statements))
    {
        // CHK-389: in a copy, what follows a constant exit is never written, so no rule judges it
        if (unrolled_depth > 0 && !block.empty() && certain_exit(block.back()).has_value())
            break;
        flatten_stmt(stmt);
    }
    auto const range = add_list(block);
    block = cc::move(outer);
    return range;
}

void sgl::check::impl::flattener::flatten_return(origin from, ast::expr_id value)
{
    // by value: flattening the value may inline a call, whose frame moves the vector this frame lives in
    auto const return_label = current()->return_label;
    auto const result_type = current()->result;
    auto result = flat_expr_id::none;
    if (ast::is_valid(value))
        result = flatten_expr(value);
    // CHK-345: the entry point's own report, decided by the fused any hit before it leaves
    if (fused.has_value() && frames.size() == 1 && is_valid(result))
        result = decide_report(from, result);
    // CHK-418: the one color a pixel stage gives is its minted struct's one member
    if (is_valid(pixel_targets) && frames.size() == 1 && is_valid(result))
    {
        flat_expr_id const color[] = {result};
        result = add_expr(pixel_targets, from.expr, flat_construct{.arguments = add_list(color)});
    }
    if (is_valid(return_label))
        add_stmt(from, flat_leave{.target = return_label, .value = result});
    else
        add_stmt(from, flat_return{.value = result});
}

void sgl::check::impl::flattener::flatten_if(origin from, cc::span<ast::if_branch const> branches, bool is_const)
{
    if (branches.empty())
        return;
    auto const& first = branches.front();
    // a stray `else` was reported by the AST pass, and no sound body holds one
    if (!ast::is_valid(first.condition))
    {
        mark_failed();
        return;
    }
    auto const condition = flatten_expr(first.condition);
    auto const rest = branches.subspan({.offset = 1, .size = branches.size() - 1});
    // CHK-356: only the side a constant takes is flattened, so nothing judges what the other side would use
    if (auto const taken = constant_condition(condition); taken.has_value())
    {
        if (taken.value())
            return add_taken(from, granting(first.condition, [&] { return flatten_body(first.then); }));
        if (!rest.empty() && !ast::is_valid(rest.front().condition))
            return add_taken(from, flatten_body(rest.front().then));
        return flatten_if(from, rest, is_const);
    }
    // CHK-425: a `const if` keeps no branch it cannot decide
    if (is_const)
    {
        unroll_reports.push_back({.file = file(),
                                  .where = c.span_of(file(), first.condition),
                                  .detail = "a const if decides each condition once the function is inlined: a "
                                            "literal, a const, an option, a literal argument, or a pure builtin "
                                            "of those",
                                  .site = innermost_call()});
        mark_failed();
        meets_error = true;
        return;
    }
    auto const then_body = granting(first.condition, [&] { return flatten_body(first.then); });

    auto else_body = ast::range_of<flat_stmt_id>();
    if (!rest.empty() && !ast::is_valid(rest.front().condition))
        else_body = flatten_body(rest.front().then);
    else if (!rest.empty())
    {
        auto const outer = cc::move(block);
        block = {};
        flatten_if(from, rest);
        else_body = add_list(block);
        block = cc::move(outer);
    }
    add_stmt(from, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
}

void sgl::check::impl::flattener::flatten_assign(origin from, ast::assign_stmt const& assign)
{
    for (auto const& t : c.out.texel_stores)
        if (t.place == assign.target && t.file == file())
            return flatten_texel_store(from, assign, t);
    if (is_swizzled(assign.target) && sgl::is_valid(assign.op))
        return flatten_swizzle_assign(from, assign, c.text_of(file(), c.file_of(file()).at(assign.op).where));
    auto const place = flatten_expr(assign.target);
    auto value = flatten_expr(assign.value);
    auto const op = sgl::is_valid(assign.op) ? c.text_of(file(), c.file_of(file()).at(assign.op).where) : "";
    if (!is_valid(place) || !is_valid(value) || op.empty())
    {
        mark_failed();
        return;
    }
    if (op != "=")
    {
        type_id const types[] = {entry.at(place).type, entry.at(value).type};
        auto const callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
        if (!is_valid(callee))
        {
            mark_failed();
            return;
        }
        flat_expr_id const arguments[] = {read_of_place(place, assign.target), value};
        value = builtin_call(assign.value, callee, arguments);
    }
    judge_decision_store(place, assign.target);
    add_stmt(from, flat_assign{.place = place, .value = value});
}

void sgl::check::impl::flattener::flatten_texel_store(origin from, ast::assign_stmt const& assign, texel_store const& t)
{
    auto const& store = c.out.call_records[t.store];
    if (t.load < 0)
    {
        add_stmt(from, flat_eval{.value = flatten_bound_call(assign.target, checked_module::void_type, store)});
        return;
    }
    auto const& load = c.out.call_records[t.load];
    auto coordinates = flatten_written(c.out.at(load.written));
    for (auto i = isize(0); i < coordinates.size(); ++i)
    {
        if (!is_valid(coordinates[i]))
        {
            mark_failed();
            return;
        }
        if (i == 0)
        {
            coordinates[i] = with_bound_index(coordinates[i], assign.target);
            continue;
        }
        if (is_substitutable(coordinates[i]))
            continue;
        auto const local = add_local(local_kind::temporary, "coordinate", entry.at(coordinates[i]).type);
        add_stmt(from, flat_let{.local = local, .value = coordinates[i]});
        coordinates[i] = local_ref(local, assign.target);
    }
    auto const texel = tables().type_at(assign.target);
    auto const loaded = target_call(assign.target, texel, load.callee, coordinates, c.out.at(load.slots));
    auto const value = flatten_expr(assign.value);
    auto const op = c.text_of(file(), c.file_of(file()).at(assign.op).where);
    type_id const types[] = {texel, is_valid(value) ? entry.at(value).type : type_id::none};
    auto const operator_callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
    if (!is_valid(loaded) || !is_valid(value) || !is_valid(operator_callee))
    {
        mark_failed();
        return;
    }
    flat_expr_id const operands[] = {loaded, value};
    auto const combined = builtin_call(assign.value, operator_callee, operands);

    // the store's written arguments are the load's, then the value
    auto values = cc::vector<flat_expr_id>();
    for (auto const coordinate : coordinates)
        values.push_back(again(coordinate, assign.target));
    values.push_back(combined);
    add_stmt(from, flat_eval{.value = target_call(assign.target, checked_module::void_type, store.callee, values,
                                                  c.out.at(store.slots))});
}

auto sgl::check::impl::flattener::read_of_place(flat_expr_id place, ast::expr_id target) -> flat_expr_id
{
    // a `mut` parameter names a place whose indices were evaluated when it was bound (`pin_place`)
    if (is_valid(place) && ast().at(target).node.is<ast::name>())
        return again(place, target);
    if (has_array_index(place))
        return reread(place, target);
    if (!is_valid(place) || !entry.at(place).node.is<flat_buffer_element>())
        return flatten_expr(target);

    // by value: evaluating once adds nodes, and the arrays move
    auto const x = entry.at(place);
    auto const element = x.node.as<flat_buffer_element>();
    auto const& indexed = ast().at(target).node.as<ast::index>();
    auto const arguments = ast().at(indexed.arguments);
    if (arguments.size() != 1)
        return fail();

    auto const once = evaluate_once(element.index, "index", arguments[0].value);
    entry.exprs[index_of(place)].node = flat_buffer_element{.buffer = element.buffer, .index = once.first};
    auto const buffer = entry.at(element.buffer);
    auto const buffer_again = add_expr(buffer.type, indexed.object, buffer.node);
    auto const index_again = once.later == once.first ? again(once.first, arguments[0].value) : once.later;
    return add_expr(x.type, target, flat_buffer_element{.buffer = buffer_again, .index = index_again});
}

auto sgl::check::impl::flattener::has_array_index(flat_expr_id id) const -> bool
{
    for (auto depth = 0; is_valid(id) && depth < k_max_inline_depth; ++depth)
    {
        auto const& node = entry.at(id).node;
        if (node.is<flat_element>())
            return true;
        auto const* const member = node.try_as<flat_member>();
        if (member == nullptr)
            return false;
        id = member->object;
    }
    return false;
}

auto sgl::check::impl::flattener::reread(flat_expr_id id, ast::expr_id from) -> flat_expr_id
{
    // by value: evaluating once adds nodes, and the arrays move
    auto const x = entry.at(id);
    if (auto const* const member = x.node.try_as<flat_member>())
    {
        auto const index = member->member;
        auto const letters = member->letters;
        auto const object = reread(member->object, from);
        return add_expr(x.type, from, flat_member{.object = object, .member = index, .letters = letters});
    }
    if (auto const* const element = x.node.try_as<flat_element>())
    {
        auto const object = reread(element->object, from);
        auto const once = evaluate_once(element->index, "index", from);
        entry.exprs[index_of(id)].node = flat_element{.object = element->object, .index = once.first};
        auto const index = once.later == once.first ? again(once.first, from) : once.later;
        return add_expr(x.type, from, flat_element{.object = object, .index = index});
    }
    // a local and a binding's member, `@workgroup` memory among them, hold nothing to evaluate
    if (x.node.is<flat_local_ref>() || x.node.is<flat_binding_member>())
        return again(id, from);
    return fail();
}

auto sgl::check::impl::flattener::innermost_call() const -> call_site
{
    auto const chain = entry.at(frames.back().chain);
    return chain.empty() ? call_site{} : chain[chain.size() - 1];
}

auto sgl::check::impl::flattener::constant_int(flat_expr_id bound) const -> cc::optional<i32>
{
    if (!is_valid(bound) || !is_constant(c.out, entry, bound))
        return {};
    auto const o = evaluate_constant(c.out, entry, bound);
    if (o.status != run_status::ok || o.result.leaves.size() != 1 || o.result.leaves[0].kind != value_kind::scalar_int)
        return {};
    return o.result.leaves[0].as_int();
}

auto sgl::check::impl::flattener::certain_exit(flat_stmt_id id) const -> cc::optional<label_id>
{
    auto const& node = entry.at(id).node;
    if (auto const* const leave = node.try_as<flat_leave>())
        return leave->target;
    if (auto const* const next = node.try_as<flat_continue>())
        return next->target;
    if (node.is<flat_return>())
        return label_id::none;
    if (auto const* const b = node.try_as<flat_block>())
    {
        auto const exit = certain_exit(b->body);
        if (exit.has_value() && exit.value() == b->label)
            return {};
        return exit;
    }
    if (auto const* const branch = node.try_as<flat_if>())
    {
        auto const then_exit = certain_exit(branch->then_body);
        if (then_exit.has_value() && then_exit == certain_exit(branch->else_body))
            return then_exit;
    }
    return {};
}

auto sgl::check::impl::flattener::certain_exit(ast::range_of<flat_stmt_id> body) const -> cc::optional<label_id>
{
    for (auto const stmt : entry.at(body))
        if (auto const exit = certain_exit(stmt); exit.has_value())
            return exit;
    return {};
}

void sgl::check::impl::flattener::flatten_unrolled_for(origin from,
                                                       ast::stmt_id id,
                                                       ast::for_stmt const& loop,
                                                       flat_expr_id first,
                                                       flat_expr_id end)
{
    if (!is_valid(first) || !is_valid(end))
    {
        mark_failed();
        return;
    }
    end = within_max_iterations(id, first, end, loop.iterable);
    auto const low = constant_int(first);
    auto const high = constant_int(end);
    auto const range_file = file();
    auto const range_where = c.span_of(range_file, loop.iterable);
    if ((!low.has_value() || !high.has_value()) && looped_around.has_value())
    {
        auto const& around = looped_around.value();
        unroll_reports.push_back(
            {.kind = diagnostic_kind::unroll_too_long,
             .file = range_file,
             .where = range_where,
             .detail = cc::format("an unrolled for runs over a constant range, and this one is no constant inside "
                                  "the unrolled for over `{}`, which is written as a loop, so this one is written "
                                  "as a loop too",
                                  c.text_of(around.file, around.where)),
             .site = innermost_call()});
        return flatten_fallen_back_for(from, id, loop, first, end);
    }
    if (!low.has_value() || !high.has_value())
    {
        unroll_reports.push_back({.file = range_file,
                                  .where = range_where,
                                  .detail = "an unrolled for runs over a constant range: both bounds are int "
                                            "constants once the function is inlined, such as `0 ..< taps` with "
                                            "`taps` a const",
                                  .site = innermost_call()});
        mark_failed();
        meets_error = true;
        return;
    }
    // in 64 bits, since a range may hold more values than an int counts
    auto const count = cc::max(i64(0), i64(high.value()) - i64(low.value()));
    // the copies around it are at most k_max_unrolled, so this cannot overflow either
    auto const copies = count * unrolled_copies;
    if (copies > k_max_unrolled)
    {
        auto const detail
            = unrolled_copies == 1
                ? cc::format("an unrolled for writes its body at most {} times, and this range holds {}, so it is "
                             "written as a loop",
                             k_max_unrolled, count)
                : cc::format("an unrolled for writes its body at most {} times, counted once per copy of the "
                             "unrolled loops around it, and this range of {} inside {} copies would write {}, so "
                             "it is written as a loop",
                             k_max_unrolled, count, unrolled_copies, copies);
        unroll_reports.push_back({.kind = diagnostic_kind::unroll_too_long,
                                  .file = range_file,
                                  .where = range_where,
                                  .detail = detail,
                                  .site = innermost_call()});
        return flatten_fallen_back_for(from, id, loop, first, end);
    }

    auto const walk = take_walk();
    auto const type = entry.at(first).type;
    auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
    auto const index_from = ast::is_valid(loop.variable) ? loop.variable : loop.iterable;
    // every read of the variable is the literal of the copy being written, which a copy rebinds
    auto const slot = current()->bound.size();
    if (n != nullptr)
        current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}});
    // what a copy binds is its own: a lookup takes the first binding of a name, so the next copy must not see it
    auto const bound_before_body = current()->bound.size();

    auto const label = add_label("unrolled");
    auto const outer = cc::move(block);
    block = {};
    auto const copies_around = unrolled_copies;
    unrolled_copies = copies;
    for (auto i = i64(0); i < count; ++i)
    {
        if (n != nullptr)
        {
            auto const k = add_expr(type, index_from, flat_int_literal{.value = i32(i64(low.value()) + i)});
            current()->bound[slot].literal = walk.has_value() ? walked(walk.value(), k, index_from) : k;
        }
        auto const copy = add_label("unrolled_index");
        current()->loops.push_back({.loop = label, .continue_block = copy});
        ++unrolled_depth;
        auto const body = flatten_body(loop.body);
        --unrolled_depth;
        current()->loops.remove_back();
        current()->bound.resize_down_to(bound_before_body);
        add_stmt(from, flat_block{.label = copy, .body = body});
        // a copy that leaves the loop whatever runs, such as one a constant `break` ends, has no copy after it
        auto const exit = certain_exit(body);
        if (exit.has_value() && exit.value() != copy)
            break;
    }
    unrolled_copies = copies_around;
    auto const written = add_list(block);
    block = cc::move(outer);
    add_stmt(from, flat_block{.label = label, .body = written});
}

void sgl::check::impl::flattener::flatten_fallen_back_for(origin from,
                                                          ast::stmt_id id,
                                                          ast::for_stmt const& loop,
                                                          flat_expr_id first,
                                                          flat_expr_id end)
{
    auto const around = looped_around;
    looped_around = looped_unroll{.file = file(), .where = c.span_of(file(), loop.iterable)};
    flatten_looped_for(from, id, loop, first, end);
    looped_around = around;
}

auto sgl::check::impl::flattener::is_rolled(ast::stmt_id id) const -> bool
{
    auto const* const a = c.find_attribute(file(), ast().at(id).attributes, "unroll");
    if (a == nullptr)
        return false;
    auto const arguments = ast().at(a->arguments);
    return arguments.size() == 1 && ast::is_valid(arguments[0].value)
        && c.text_of(file(), c.span_of(file(), arguments[0].value)) == "false";
}

auto sgl::check::impl::flattener::is_unrolled(ast::stmt_id id) const -> bool
{
    return c.find_attribute(file(), ast().at(id).attributes, "unroll") != nullptr && !is_rolled(id);
}

void sgl::check::impl::flattener::flatten_looped_for(origin from,
                                                     ast::stmt_id id,
                                                     ast::for_stmt const& loop,
                                                     flat_expr_id first,
                                                     flat_expr_id end)
{
    auto const guard = pending_guard;
    pending_guard = {};
    if (!guard.has_value())
        if (auto const bound = max_iterations_of(id); bound.has_value() && is_valid(first) && is_valid(end))
        {
            if (!constant_int(first).has_value() || !constant_int(end).has_value())
                return flatten_bounded_for(from, id, loop, first, end, false, bound.value());
            end = within_max_iterations(id, first, end, loop.iterable);
        }
    auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
    auto const int_type = is_valid(first) ? entry.at(first).type : type_id::none;
    auto const walk = take_walk();
    auto const index = add_local(local_kind::index,
                                 walk.has_value() ? cc::string_view("k")
                                 : n != nullptr   ? c.text_of(file(), n->where)
                                                  : cc::string_view("i"),
                                 int_type);
    current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)},
                                .local = index,
                                .literal = walk.has_value()
                                             ? walked(walk.value(), local_ref(index, loop.iterable), loop.iterable)
                                             : flat_expr_id::none});

    auto const label = add_label("for");
    entry.labels[index_of(label)].is_rolled = is_rolled(id);
    auto const is_test_loop = is_test && frames.size() == 1;
    if (is_test_loop)
        test_loops.push_back(index);
    current()->loops.push_back({.loop = label});
    auto body = flatten_body(loop.body);
    current()->loops.remove_back();
    if (is_test_loop)
        test_loops.remove_back();
    if (guard.has_value())
    {
        auto const outer = cc::move(block);
        block = {};
        auto const where = loop.iterable;
        flat_stmt_id const leave[] = {make_stmt(from, flat_leave{.target = label})};
        auto const done = add_expr(bool_type(), where, flat_not{.operand = range_continues(guard.value(), index, where)});
        add_stmt(from, flat_if{.condition = done, .then_body = add_list(leave)});
        for (auto const s : entry.at(body))
            block.push_back(s);
        body = add_list(block);
        block = cc::move(outer);
    }
    add_stmt(from, flat_for{.label = label, .index = index, .first = first, .end = end, .body = body});
}

auto sgl::check::impl::flattener::max_iterations_of(ast::stmt_id id) -> cc::optional<i32>
{
    auto const* const a = c.find_attribute(file(), ast().at(id).attributes, "max_iterations");
    if (a == nullptr)
        return {};
    auto const arguments = ast().at(a->arguments);
    if (arguments.size() != 1 || !ast::is_valid(arguments[0].value))
        return {};
    return constant_int(flatten_expr(arguments[0].value));
}

auto sgl::check::impl::flattener::within_max_iterations(ast::stmt_id id,
                                                        flat_expr_id first,
                                                        flat_expr_id end,
                                                        ast::expr_id where) -> flat_expr_id
{
    auto const bound = max_iterations_of(id);
    auto const low = constant_int(first);
    auto const high = constant_int(end);
    if (!bound.has_value() || !low.has_value() || !high.has_value() || i64(high.value()) - low.value() <= bound.value())
        return end;
    return add_expr(entry.at(first).type, where, flat_int_literal{.value = low.value() + bound.value()});
}

void sgl::check::impl::flattener::flatten_bounded_for(origin from,
                                                      ast::stmt_id id,
                                                      ast::for_stmt const& loop,
                                                      flat_expr_id first,
                                                      flat_expr_id end,
                                                      bool is_inclusive,
                                                      i32 bound)
{
    auto const where = loop.iterable;
    auto const type = entry.at(first).type;
    // both bounds once, first before end, as a `for` evaluates them
    auto guard = range_guard{.is_inclusive = is_inclusive};
    if (auto const low = constant_int(first); !low.has_value() || low.value() != 0)
    {
        guard.first = add_local(local_kind::temporary, "first", type);
        add_stmt(from, flat_let{.local = guard.first, .value = first});
        CC_ASSERT(!pending_walk.has_value(), "a walk counts from zero, so only a plain range is mapped here");
        pending_walk = walk_mapping{.start = local_ref(guard.first, where), .step = 1};
    }
    guard.end = add_local(local_kind::temporary, is_inclusive ? "last" : "end", type);
    add_stmt(from, flat_let{.local = guard.end, .value = end});
    pending_guard = guard;
    auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
    flatten_looped_for(from, id, loop, literal(0), literal(bound));
}

auto sgl::check::impl::flattener::range_continues(range_guard const& guard, local_id k, ast::expr_id where) -> flat_expr_id
{
    auto const type = entry.at(k).type;
    type_id const ints[] = {type, type};
    auto const op = [&](cc::string_view spelling, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const operands[] = {a, b};
        return builtin_call(where, c.find_operator(file(), spelling, ints), operands);
    };
    auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
    auto const value = [&](flat_expr_id offset)
    { return is_valid(guard.first) ? op("+", local_ref(guard.first, where), offset) : offset; };
    if (!guard.is_inclusive)
        return op("<", value(local_ref(k, where)), local_ref(guard.end, where));
    // the first value where the range holds one, then each while the last was below `last`, so a range up to the
    // largest int runs whole and no value past `last` is computed
    auto const b = bool_type();
    auto const starts = add_expr(b, where,
                                 flat_and{.lhs = op("==", local_ref(k, where), literal(0)),
                                          .rhs = op("<=", value(literal(0)), local_ref(guard.end, where))});
    auto const goes_on = add_expr(
        b, where,
        flat_and{.lhs = op(">", local_ref(k, where), literal(0)),
                 .rhs = op("<", value(op("-", local_ref(k, where), literal(1))), local_ref(guard.end, where))});
    return add_expr(b, where, flat_or{.lhs = starts, .rhs = goes_on});
}

void sgl::check::impl::flattener::flatten_bounded_while(origin from, ast::while_stmt const& loop, i32 bound)
{
    auto const where = loop.condition;
    auto const type = int_type();
    auto const label = add_label("while");
    auto const k = add_local(local_kind::index, "k", type);
    current()->loops.push_back({.loop = label});
    auto const outer = cc::move(block);
    block = {};
    // the condition is evaluated before every iteration, inside it, so a `continue` reaches it again
    auto const condition = flatten_expr(loop.condition);
    flat_stmt_id const leave[] = {make_stmt(from, flat_leave{.target = label})};
    add_stmt(from, flat_if{.condition = add_expr(bool_type(), where, flat_not{.operand = condition}),
                           .then_body = add_list(leave)});
    auto const inner = flatten_body(loop.body);
    for (auto const s : entry.at(inner))
        block.push_back(s);
    auto const body = add_list(block);
    block = cc::move(outer);
    current()->loops.remove_back();
    auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
    add_stmt(from, flat_for{.label = label, .index = k, .first = literal(0), .end = literal(bound), .body = body});
}

void sgl::check::impl::flattener::flatten_inclusive_for(origin from,
                                                        ast::stmt_id id,
                                                        ast::for_stmt const& loop,
                                                        flat_expr_id first,
                                                        flat_expr_id last)
{
    if (!is_valid(first) || !is_valid(last))
    {
        mark_failed();
        return;
    }
    auto const int_type = entry.at(first).type;
    auto const unrolls = is_unrolled(id);
    if (auto const high = constant_int(last); high.has_value() && high.value() < 2147483647)
    {
        auto const end = add_expr(int_type, loop.iterable, flat_int_literal{.value = high.value() + 1});
        if (unrolls)
            return flatten_unrolled_for(from, id, loop, first, end);
        return flatten_looped_for(from, id, loop, first, end);
    }
    if (auto const bound = max_iterations_of(id); bound.has_value())
        return flatten_bounded_for(from, id, loop, first, last, true, bound.value());
    type_id const ints[] = {int_type, int_type};
    auto const at_most = c.find_operator(file(), "<=", ints);
    auto const equals = c.find_operator(file(), "==", ints);
    auto const plus = c.find_operator(file(), "+", ints);
    if (!is_valid(at_most) || !is_valid(equals) || !is_valid(plus))
    {
        mark_failed();
        return;
    }
    auto const where = loop.iterable;
    // both bounds once, first before last, as a `for` evaluates them
    auto const low = add_local(local_kind::temporary, "first", int_type);
    add_stmt(from, flat_let{.local = low, .value = first});
    auto const high = add_local(local_kind::temporary, "last", int_type);
    add_stmt(from, flat_let{.local = high, .value = last});

    auto const outer = cc::move(block);
    block = {};
    auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
    auto const index
        = add_local(local_kind::var, n != nullptr ? c.text_of(file(), n->where) : cc::string_view("i"), int_type);
    add_stmt(from, flat_var{.local = index, .value = local_ref(low, where)});
    current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .local = index});

    auto const label = add_label("for");
    entry.labels[index_of(label)].is_rolled = is_rolled(id);
    auto const iteration = add_label("iteration");
    auto const is_test_loop = is_test && frames.size() == 1;
    if (is_test_loop)
        test_loops.push_back(index);
    current()->loops.push_back({.loop = label, .continue_block = iteration});
    auto const body = flatten_body(loop.body);
    current()->loops.remove_back();
    if (is_test_loop)
        test_loops.remove_back();

    auto const around = cc::move(block);
    block = {};
    add_stmt(from, flat_block{.label = iteration, .body = body});
    flat_expr_id const reached[] = {local_ref(index, where), local_ref(high, where)};
    flat_stmt_id const leave[] = {make_stmt(from, flat_leave{.target = label})};
    add_stmt(from, flat_if{.condition = builtin_call(where, equals, reached), .then_body = add_list(leave)});
    flat_expr_id const next[] = {local_ref(index, where), add_expr(int_type, where, flat_int_literal{.value = 1})};
    add_stmt(from, flat_assign{.place = local_ref(index, where), .value = builtin_call(where, plus, next)});
    auto const steps = add_list(block);
    block = cc::move(around);
    add_stmt(from, flat_loop{.label = label, .body = steps});

    auto const runs = add_list(block);
    block = cc::move(outer);
    flat_expr_id const ordered[] = {local_ref(low, where), local_ref(high, where)};
    add_stmt(from, flat_if{.condition = builtin_call(where, at_most, ordered), .then_body = runs});
}

auto sgl::check::impl::flattener::take_walk() -> cc::optional<walk_mapping>
{
    auto const walk = pending_walk;
    pending_walk = {};
    return walk;
}

auto sgl::check::impl::flattener::walked(walk_mapping const& walk, flat_expr_id k, ast::expr_id where) -> flat_expr_id
{
    auto const type = entry.at(k).type;
    auto const start = constant_int(walk.start);
    auto const index = constant_int(k);
    auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
    if (start.has_value() && index.has_value())
        return literal(start.value() + index.value() * walk.step);
    type_id const ints[] = {type, type};
    auto const op = [&](cc::string_view spelling, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const operands[] = {a, b};
        return builtin_call(where, c.find_operator(file(), spelling, ints), operands);
    };
    auto const is_zero = start.has_value() && start.value() == 0;
    if (walk.step == -1)
        return op("-", is_zero ? literal(0) : again(walk.start, where), again(k, where));
    auto const scaled = walk.step == 1 ? again(k, where) : op("*", again(k, where), literal(walk.step));
    return is_zero ? scaled : op("+", again(walk.start, where), scaled);
}

void sgl::check::impl::flattener::flatten_walked_for(origin from,
                                                     ast::stmt_id id,
                                                     ast::for_stmt const& loop,
                                                     checker::range_walk const& walk)
{
    auto const& r = *walk.range;
    auto first = flatten_expr(r.first);
    auto last = flatten_expr(r.last);
    if (!is_valid(first) || !is_valid(last))
    {
        mark_failed();
        return;
    }
    auto const where = loop.iterable;
    auto const type = entry.at(first).type;
    type_id const ints[] = {type, type};
    auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
    // a part that is no constant is held in a local, so the bounds are evaluated once and in order
    auto const held = [&](flat_expr_id value, cc::string_view name)
    {
        if (constant_int(value).has_value())
            return value;
        auto const local = add_local(local_kind::temporary, name, type);
        add_stmt(from, flat_let{.local = local, .value = value});
        return local_ref(local, where);
    };
    auto const op = [&](cc::string_view spelling, flat_expr_id a, flat_expr_id b)
    {
        auto const x = constant_int(a);
        auto const y = constant_int(b);
        if (x.has_value() && y.has_value())
        {
            auto const l = i64(x.value());
            auto const m = i64(y.value());
            auto const v = spelling == "+" ? l + m : spelling == "-" ? l - m : spelling == "*" ? l * m : l / m;
            return literal(i32(v));
        }
        flat_expr_id const operands[] = {a, b};
        return builtin_call(where, c.find_operator(file(), spelling, ints), operands);
    };
    first = held(first, "first");
    last = held(last, "last");
    auto const is_inclusive = sgl::is_valid(r.op) && c.text_of(file(), c.file_of(file()).at(r.op).where) == "..=";
    auto count
        = held(op("-", is_inclusive ? op("+", again(last, where), literal(1)) : again(last, where), again(first, where)),
               "count");
    auto start = first;
    auto step = 1;
    for (auto const s : walk.steps)
    {
        if (!ast::is_valid(s))
        {
            // the last value first: `start + (count - 1) * step`
            start = held(op("+", again(start, where), op("*", op("-", again(count, where), literal(1)), literal(step))),
                         "start");
            step = -step;
            continue;
        }
        auto const by = constant_int(flatten_expr(s));
        if (!by.has_value() || by.value() < 1)
        {
            mark_failed();
            return;
        }
        // as many values as a step of `by` reaches, rounded up; a count of none or fewer stays one of none
        count = held(op("/", op("+", again(count, where), literal(by.value() - 1)), literal(by.value())), "count");
        step *= by.value();
    }
    pending_walk = walk_mapping{.start = start, .step = step};
    if (is_unrolled(id))
        return flatten_unrolled_for(from, id, loop, literal(0), count);
    flatten_looped_for(from, id, loop, literal(0), count);
}

void sgl::check::impl::flattener::flatten_for(origin from, ast::stmt_id id, ast::for_stmt const& loop)
{
    auto const walk = ast::is_valid(loop.iterable) ? c.range_walk_of(file(), loop.iterable) : checker::range_walk();
    auto const* const r = walk.range;
    if (r == nullptr)
    {
        mark_failed();
        return;
    }
    if (!walk.steps.empty())
        return flatten_walked_for(from, id, loop, walk);
    auto const first = flatten_expr(r->first);
    auto const end = flatten_expr(r->last);
    if (sgl::is_valid(r->op) && c.text_of(file(), c.file_of(file()).at(r->op).where) == "..=")
        return flatten_inclusive_for(from, id, loop, first, end);
    if (is_unrolled(id))
        return flatten_unrolled_for(from, id, loop, first, end);
    flatten_looped_for(from, id, loop, first, end);
}

void sgl::check::impl::flattener::flatten_stmt(ast::stmt_id id)
{
    auto const& s = ast().at(id);
    auto const from = origin{.file = file(), .stmt = id};
    auto const at = cursor_scope(*this, from);
    if (auto const* const let = s.node.try_as<ast::let_stmt>())
    {
        auto const* const name = ast::is_valid(let->pattern) ? ast().at(let->pattern).node.try_as<ast::name>() : nullptr;
        if (name == nullptr)
        {
            mark_failed();
            return;
        }
        // minted ahead of its value, so the name the program wrote is its own and a callee inlined there mints past it
        auto const local = add_local(let->is_mut ? local_kind::var : local_kind::let, c.text_of(file(), name->where),
                                     tables().type_at(let->pattern));
        // CHK-341: a `let mut` of `undefined()` is that local, with nothing to copy
        auto const is_undefined
            = ast::is_valid(let->value) && tables().target_at(let->value).kind == target_kind::undefined_value;
        entry.locals[index_of(local)].is_undefined = is_undefined;
        // CHK-55: a `let mut` without a value holds nothing until it is assigned, which judge_assignments holds to
        auto const value = !is_undefined && (ast::is_valid(let->value) || !let->is_mut) ? flatten_expr(let->value)
                                                                                        : flat_expr_id::none;
        current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .local = local});
        if (let->is_mut)
            add_stmt(from, flat_var{.local = local, .value = value});
        else
            add_stmt(from, flat_let{.local = local, .value = value});
        return;
    }
    if (auto const* const assign = s.node.try_as<ast::assign_stmt>())
        return flatten_assign(from, *assign);
    if (auto const* const chain = s.node.try_as<ast::if_stmt>())
        return flatten_if(from, ast().at(chain->branches), chain->is_const);
    if (auto const* const loop = s.node.try_as<ast::for_stmt>())
        return flatten_for(from, id, *loop);
    if (auto const* const loop = s.node.try_as<ast::while_stmt>())
    {
        if (auto const bound = max_iterations_of(id); bound.has_value())
            return flatten_bounded_while(from, *loop, bound.value());
        auto const label = add_label("while");
        auto const condition = flatten_expr(loop->condition);
        current()->loops.push_back({.loop = label});
        auto const body = flatten_body(loop->body);
        current()->loops.remove_back();
        return add_stmt(from, flat_while{.label = label, .condition = condition, .body = body});
    }
    // CHK-420: a scope's locals are minted apart already, so its statements join the block it stands in
    if (auto const* const scoped = s.node.try_as<ast::scope_stmt>())
    {
        for (auto const stmt : ast().at(scoped->body.statements))
        {
            if (unrolled_depth > 0 && !block.empty() && certain_exit(block.back()).has_value())
                break;
            flatten_stmt(stmt);
        }
        return;
    }
    if (auto const* const print = s.node.try_as<ast::print_stmt>())
    {
        auto const* const literal = ast().at(print->message).node.try_as<ast::literal>();
        if (literal == nullptr || literal->kind != ast::literal_kind::quoted)
        {
            auto const value = flatten_expr(print->message);
            return add_stmt(from, flat_print{.values = add_list(cc::span<flat_expr_id const>(&value, 1))});
        }
        auto const message = add_message(print->message);
        return add_stmt(from, flat_print{.message = message, .values = flatten_message_values(print->message)});
    }
    // A test in a function body never runs where it stands (CHK-224), and a `require` and a `const` are no code.
    if (auto const* const d = s.node.try_as<ast::decl_stmt>();
        d != nullptr && ast::is_valid(d->declaration)
        && (ast().at(d->declaration).node.is<ast::test_decl>() || ast().at(d->declaration).node.is<ast::require_decl>()
            || ast().at(d->declaration).node.is<ast::const_decl>()))
        return;
    if (auto const* const a = s.node.try_as<ast::assert_stmt>())
        return flatten_check(from, a->condition, true, a->message);
    // CHK-427: a body in a target's own text is a call the emitter writes as that text
    if (c.tagged_text_token(file(), id) >= 0)
        return flatten_target_text(from, id);

    auto const* const e = s.node.try_as<ast::expr_stmt>();
    if (e == nullptr || !ast::is_valid(e->value))
    {
        mark_failed();
        return;
    }
    flatten_expr_stmt(from, e->value);
}

void sgl::check::impl::flattener::flatten_expr_stmt(origin from, ast::expr_id value)
{
    auto const& x = ast().at(value);
    auto const& loops = current()->loops;
    if (auto const* const r = x.node.try_as<ast::return_expr>())
        return flatten_return(from, r->value);
    if (auto const* const b = x.node.try_as<ast::break_expr>())
    {
        if (loops.empty())
        {
            mark_failed();
            return;
        }
        if (!ast::is_valid(b->value))
            return add_stmt(from, flat_leave{.target = loops.back().loop});
        // by value: flattening the value may open a loop of its own
        auto const target = loops.back().value_block;
        is_failed = is_failed || !is_valid(target);
        return add_stmt(from, flat_leave{.target = target, .value = flatten_expr(b->value)});
    }
    if (x.node.is<ast::discard_expr>())
    {
        // CHK-277: only a pixel entry point may reach it, which is known once the whole body is inlined
        discards.push_back({.file = file(), .expr = value});
        return add_stmt(from, flat_discard{});
    }
    if (x.node.is<ast::continue_expr>())
    {
        if (loops.empty())
        {
            mark_failed();
            return;
        }
        if (is_valid(loops.back().continue_block))
            return add_stmt(from, flat_leave{.target = loops.back().continue_block});
        return add_stmt(from, flat_continue{.target = loops.back().loop});
    }
    if (auto const* const loop = x.node.try_as<ast::loop_expr>())
    {
        auto const label = add_label("loop");
        current()->loops.push_back({.loop = label});
        auto const body = flatten_body(loop->body);
        current()->loops.remove_back();
        return add_stmt(from, flat_loop{.label = label, .body = body});
    }
    if (auto const* const y = x.node.try_as<ast::yield_expr>())
    {
        auto const& blocks = current()->value_blocks;
        if (blocks.empty())
        {
            mark_failed();
            return;
        }
        // by value: flattening the value may open a value block of its own
        auto const target = blocks.back();
        auto const value = flatten_expr(y->value);
        return add_stmt(from, flat_leave{.target = target, .value = value});
    }
    if (auto const* const c = x.node.try_as<ast::case_expr>())
        return add_stmt(from, flatten_case_parts(*c, label_id::none));
    if (auto const* const i = x.node.try_as<ast::if_expr>())
        return flatten_if_branches(from, ast().at(i->branches), label_id::none);

    // CHK-225: a line of type bool of the test's own body is a check.
    if (is_test && frames.size() == 1 && tables().type_at(value) == bool_type())
        return flatten_check(from, value, false);

    // What is left is a call, and one that returns `void` is a block that is a statement.
    // A line that is no call computes nothing, which the check pass reported; it is evaluated and dropped.
    auto const* const call = x.node.try_as<ast::call>();
    if (call == nullptr)
        return add_stmt(from, flat_eval{.value = flatten_expr(value)});
    auto const& where = tables().target_at(value);
    auto const record = tables().call_at(value);
    if (where.kind == target_kind::overload && record >= 0 && is_inlined(where.symbol)
        && tables().type_at(value) == checked_module::void_type)
    {
        auto const& r = c.out.call_records[record];
        if (is_diff_call(where.symbol))
        {
            auto const derived = flatten_diff(value, checked_module::void_type, r);
            if (!is_valid(derived))
                return;
            auto const b = entry.at(derived).node.as<flat_block>();
            return add_stmt(from, flat_block{.label = b.label, .body = b.body});
        }
        auto const values = flatten_written(c.out.at(r.written));
        // a function argument is handed over as a closure, and a generic callee takes its type arguments
        auto const handed = written_closures;
        auto const inlined
            = inline_bound(value, where.symbol, values, c.out.at(r.slots), handed, c.out.at(r.type_arguments));
        return add_stmt(from, flat_block{.label = inlined.label, .body = inlined.body});
    }
    // CHK-319: a call through a parameter of function type that gives `void` is the block it inlines, a statement
    if (record >= 0 && !is_valid(c.out.call_records[record].callee)
        && tables().type_at(value) == checked_module::void_type)
    {
        auto const inlined = flatten_expr(value);
        if (!is_valid(inlined))
            return;
        if (auto const* const b = entry.at(inlined).node.try_as<flat_block>())
        {
            auto const block = *b;
            return add_stmt(from, flat_block{.label = block.label, .body = block.body});
        }
        return add_stmt(from, flat_eval{.value = inlined});
    }
    // Any other call has a value, which is evaluated where it stands and dropped.
    add_stmt(from, flat_eval{.value = flatten_expr(value)});
}

void sgl::check::impl::flattener::flatten_target_text(origin from, ast::stmt_id stmt)
{
    // the run's first statement stands for all of it, one alternative per language
    auto const language = compiled_for().language;
    auto chosen = -1;
    auto function = symbol_id::none;
    for (auto i = isize(0); i < c.out.target_texts.size(); ++i)
    {
        auto const& t = c.out.target_texts[i];
        if (t.file != file() || t.first != stmt)
            continue;
        function = t.function;
        // a description writes no text, and a body's footprint is its parameters' whichever language it is in
        if (t.language == language || (language == shading_language::sgl && !is_test && chosen < 0))
            chosen = i32(i);
    }
    if (!is_valid(function))
        return;

    auto const literal = ast().at(stmt).node.as<ast::expr_stmt>().value;
    auto const& name = c.out.at(function).name;
    if (chosen < 0)
    {
        auto const detail = language == shading_language::sgl
                              ? cc::format("a test reaches {}, whose body is a target's own text, which no "
                                           "interpreter runs",
                                           name)
                              : cc::format("{} has no body in {}, the language this compile writes", name,
                                           k_shading_language_names[isize(language)]);
        unroll_reports.push_back({.kind = diagnostic_kind::invalid_target_text,
                                  .file = file(),
                                  .where = c.span_of(file(), stmt),
                                  .detail = detail,
                                  .site = innermost_call()});
        mark_failed();
        meets_error = true;
        return;
    }

    // every parameter in order, as the footprint reads a call's arguments against its callee's parameters
    auto arguments = cc::vector<flat_expr_id>();
    for (auto const& p : c.out.at(c.out.functions[c.out.at(function).info].parameters))
    {
        auto const where = target{.kind = target_kind::parameter, .index = i32(p.field)};
        auto value = flat_expr_id::none;
        for (auto const& b : current()->bound)
            if (b.where == where && is_valid(b.literal))
                value = again(b.literal, literal);
            else if (b.where == where && is_valid(b.local))
                value = local_ref(b.local, literal);
        if (!is_valid(value))
        {
            mark_failed();
            return;
        }
        arguments.push_back(value);
    }

    // CHK-427: the text uses what the `has` of the branches around it granted, which is how a compile learns what it
    // asks of the device, a shader model among it
    if (!granted.is_empty())
    {
        auto within = cc::vector<symbol_id>();
        for (auto const& fr : frames)
            within.push_back(fr.function);
        feature_uses.push_back(
            {.file = file(), .call = literal, .features = granted, .within = cc::move(within), .granted = granted});
    }

    auto const return_label = current()->return_label;
    auto const result_type = current()->result;
    auto const is_void = !is_valid(result_type) || result_type == checked_module::void_type;
    auto const call = add_expr(is_void ? checked_module::void_type : result_type, literal,
                               flat_call{.callee = function,
                                         .is_pure = c.out.functions[c.out.at(function).info].is_pure,
                                         .arguments = add_list(arguments),
                                         .text = chosen});
    if (is_void)
        return add_stmt(from, flat_eval{.value = call});
    if (is_valid(return_label))
        add_stmt(from, flat_leave{.target = return_label, .value = call});
    else
        add_stmt(from, flat_return{.value = call});
}

void sgl::check::impl::flattener::judge_text_types()
{
    if (c.out.target_type_branches.empty())
        return;
    auto const target = compiled_for();
    // what each text type this tree holds is on this compile: -1 for not yet seen, -2 for fields, else its spelling's
    // position in `entry.text_types`, or -3 where it has none
    auto decided = cc::vector<cc::pair<type_id, i32>>();
    auto const report_at = [&](flat_expr const& x, cc::string detail)
    {
        auto const chain = entry.at(x.inlined_through);
        unroll_reports.push_back({.kind = diagnostic_kind::invalid_target_text,
                                  .file = x.from.file,
                                  .where = c.span_of(x.from.file, x.from.expr),
                                  .detail = cc::move(detail),
                                  .site = chain.empty() ? call_site{} : chain[chain.size() - 1]});
        is_failed = true;
        meets_error = true;
    };
    auto const decide = [&](flat_expr const& x, type_id type) -> i32
    {
        for (auto const& d : decided)
            if (d.first == type)
                return d.second;
        auto const& name = c.out.name_of(type);
        auto const chosen = chosen_type_branch(c.out, c.out.at(type).symbol, target);
        auto result = -3;
        if (chosen >= 0 && c.out.target_type_branches[chosen].is_fields)
            result = -2;
        else if (chosen < 0)
            report_at(x, cc::format("{} has no branch this compile takes, so it has no type here", name));
        else if (target.language == shading_language::sgl)
        {
            // a description writes no text, and a test runs on no target's type
            if (is_test)
                report_at(x, cc::format("a test reaches {}, which this compile spells in a target's own text, and no "
                                        "interpreter runs that",
                                        name));
        }
        else
        {
            auto const& branch = c.out.target_type_branches[chosen];
            for (auto i = isize(0); i < branch.languages.size() && result < 0; ++i)
                if (branch.languages[i] == target.language)
                {
                    result = i32(entry.text_types.size());
                    entry.text_types.push_back({.type = type, .spelling = branch.spellings[i]});
                }
            if (result < 0)
                report_at(x, cc::format("{} has no spelling in {}, the language this compile writes", name,
                                        k_shading_language_names[isize(target.language)]));
        }
        decided.push_back({type, result});
        return result;
    };
    auto const is_text_type = [&](type_id type)
    {
        return is_valid(type) && type != checked_module::error_type && c.out.at(type).kind == type_kind::structure
            && c.out.at(type).is_text_type;
    };
    for (auto i = isize(0); i < entry.exprs.size(); ++i)
    {
        auto const& x = entry.exprs[i];
        if (is_text_type(x.type) && decide(x, x.type) != -2 && x.node.is<flat_construct>())
            report_at(x, cc::format("{} is a target's own type on this compile, which no SGL constructor builds",
                                    c.out.name_of(x.type)));
        if (auto const* const m = x.node.try_as<flat_member>(); m != nullptr && is_valid(m->object))
        {
            auto const object = entry.exprs[index_of(m->object)].type;
            if (is_text_type(object) && decide(x, object) != -2)
                report_at(x, cc::format("{} is a target's own type on this compile, and its fields are its SGL "
                                        "branch's alone",
                                        c.out.name_of(object)));
        }
    }
}

void sgl::check::impl::flattener::place_target_layouts()
{
    if (c.target_layouts.empty() && c.unplaced_layouts.empty())
        return;
    auto const placed = [&](type_id type) -> type_id
    {
        for (auto const& [from, to] : c.target_layouts)
            if (from == type)
                return to;
        return type;
    };
    // CHK-526: where each field the check judged stands in the branch's record, by the branch that made it
    auto const places_of = [&](type_id to) -> cc::span<layout_place const>
    {
        for (auto const& b : c.out.target_type_branches)
            if (b.layout == to)
                return b.places;
        return {};
    };
    auto const report_at = [&](flat_expr const& x, diagnostic_kind kind, cc::string detail)
    {
        auto const chain = entry.at(x.inlined_through);
        unroll_reports.push_back({.kind = kind,
                                  .file = x.from.file,
                                  .where = c.span_of(x.from.file, x.from.expr),
                                  .detail = cc::move(detail),
                                  .site = chain.empty() ? call_site{} : chain[chain.size() - 1]});
        is_failed = true;
        meets_error = true;
    };
    // a node beside the construction it stands in, attributed to it
    auto const add_beside = [&](isize at, type_id type, decltype(flat_expr::node) node) -> flat_expr_id
    {
        auto const origin = entry.exprs[at].from;
        auto const through = entry.exprs[at].inlined_through;
        entry.exprs.push_back({.type = type, .from = origin, .inlined_through = through, .node = cc::move(node)});
        return flat_expr_id(entry.exprs.size() - 1);
    };
    auto const call_of = [&](isize at, cc::string_view op, flat_expr_id lhs, flat_expr_id rhs) -> flat_expr_id
    {
        auto const type = entry.at(lhs).type;
        type_id const types[] = {type, type};
        auto const callee = c.find_operator(entry.exprs[at].from.file, op, types);
        if (!is_valid(callee))
            return flat_expr_id::none;
        flat_expr_id const arguments[] = {lhs, rhs};
        return add_beside(at, type,
                          flat_call{.callee = callee,
                                    .intrinsic = c.out.at(callee).intrinsic,
                                    .is_pure = true,
                                    .arguments = add_list(arguments)});
    };
    auto const literal_of = [&](isize at, type_id type, u32 value)
    { return add_beside(at, type, flat_int_literal{.value = i32(value), .is_unsigned = true, .is_hex = true}); };

    auto is_reported = false;
    for (auto i = isize(0); i < entry.exprs.size(); ++i)
    {
        if (!is_reported && c.unplaced_layouts.values().contains(entry.exprs[i].type))
        {
            report_at(entry.exprs[i], diagnostic_kind::invalid_target_text,
                      cc::format("{} has no branch this compile takes, so it has no layout here",
                                 c.out.name_of(entry.exprs[i].type)));
            is_reported = true;
        }
        if (auto* const m = entry.exprs[i].node.try_as<flat_member>();
            m != nullptr && is_valid(m->object) && !m->is_swizzle())
        {
            auto const object = entry.exprs[index_of(m->object)].type;
            if (auto const to = placed(object); to != object)
            {
                auto const& place = places_of(to)[m->member];
                if (place.member < 0 || place.bits != 0)
                    report_at(entry.exprs[i], diagnostic_kind::unsupported_yet,
                              cc::format("a read or a write of {}.{} alone, which this compile {}",
                                         c.out.name_of(object), c.out.at(c.out.at(object).members)[m->member].name,
                                         place.member < 0 ? "has no field of" : "packs into the bits of a word"));
                else
                    m->member = place.member;
            }
        }
        auto* const k = entry.exprs[i].node.try_as<flat_construct>();
        if (k == nullptr)
            continue;
        auto const from = entry.exprs[i].type;
        auto const to = placed(from);
        if (to == from)
            continue;
        auto const arguments = cc::vector<flat_expr_id>::create_copy_of(entry.at(k->arguments));
        auto const places = cc::vector<layout_place>::create_copy_of(places_of(to));
        // its arguments are written in the branch's order, and an absent field's is dropped, which no effect may tell
        auto with_effect = 0;
        auto is_dropped_effect = false;
        for (auto a = isize(0); a < arguments.size(); ++a)
        {
            auto const has_effect = calls_impure(arguments[a], 0);
            with_effect += has_effect ? 1 : 0;
            is_dropped_effect = is_dropped_effect || (has_effect && places[a].member < 0);
        }
        if (with_effect >= 2 || is_dropped_effect)
        {
            report_at(entry.exprs[i], diagnostic_kind::unsupported_yet,
                      cc::format("a construction of {} whose arguments' effects this compile would {}",
                                 c.out.name_of(from), is_dropped_effect ? "drop" : "write in another order"));
            continue;
        }
        // CHK-526: each member of the record from the arguments it holds, a packed word ORing its fields in place
        auto members = cc::vector<flat_expr_id>::create_filled(c.out.at(to).members.count, flat_expr_id::none);
        for (auto a = isize(0); a < arguments.size(); ++a)
        {
            auto const& place = places[a];
            if (place.member < 0)
                continue;
            auto value = arguments[a];
            if (place.bits != 0)
            {
                auto const type = entry.at(value).type;
                auto const mask = place.bits == 32 ? 0xffff'ffffu : (1u << u32(place.bits)) - 1u;
                value = call_of(i, "&", value, literal_of(i, type, mask));
                if (place.offset != 0)
                    value = call_of(i, "<<", value, literal_of(i, type, u32(place.offset)));
                if (is_valid(members[place.member]))
                    value = call_of(i, "|", members[place.member], value);
            }
            members[place.member] = value;
        }
        // a word whose fields are all absent, and none is, is zero
        for (auto at = isize(0); at < members.size(); ++at)
            if (!is_valid(members[at]))
                members[at] = literal_of(i, c.out.at(c.out.at(to).members)[at].type, 0u);
        auto const list = add_list(members);
        entry.exprs[i].node.as<flat_construct>().arguments = list;
    }
    // the types last, since a field is renumbered from the type its object had
    for (auto& x : entry.exprs)
        x.type = placed(x.type);
    for (auto& l : entry.locals)
        l.type = placed(l.type);
}
