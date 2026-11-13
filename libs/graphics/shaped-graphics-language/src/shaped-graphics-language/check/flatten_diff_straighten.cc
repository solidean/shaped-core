#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/differentiator.hh>

// PROTOTYPE: Reverse mode's first half: the body as straight structured code, every leave a flag and every overwrite saved.

namespace sgl::check::impl
{
auto differentiator::hoist(cc::string_view name, type_id type, flat_expr_id value) -> local_id
{
    auto const local = f.add_local(local_kind::var, name, type);
    hoisted.push_back(f.make_stmt(where, flat_var{.local = local, .value = value}));
    return local;
}

auto differentiator::hoist_declarations(cc::span<flat_stmt_id const> statements) -> cc::vector<flat_stmt_id>
{
    auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
    return collect(
        [&]
        {
            for (auto const id : copy)
            {
                auto const s = f.entry.at(id);
                auto const declare = [&](local_id local, flat_expr_id value, bool is_let)
                {
                    auto& l = f.entry.locals[index_of(local)];
                    l.kind = local_kind::var;
                    l.is_mut = true;
                    auto leaves = cc::vector<scalar>();
                    if (!leaves_of(l.type, 0.0f, leaves))
                        return (void)failed(cc::format("a local of {}", m().name_of(l.type)));
                    hoisted.push_back(f.make_stmt(where, flat_var{.local = local, .value = zero(l.type)}));
                    if (is_differentiable(l.type))
                        tracked.insert(i32(local));
                    if (is_let && loop_depth == 0)
                        single_assigned.insert(i32(local));
                    if (is_valid(value))
                        restate(id, flat_assign{.place = ref(local), .value = value});
                };
                if (auto const* const n = s.node.try_as<flat_let>())
                    declare(n->local, n->value, true);
                else if (auto const* const n = s.node.try_as<flat_var>())
                    declare(n->local, n->value, false);
                else if (auto const* const n = s.node.try_as<flat_if>())
                {
                    auto const then_body = f.add_list(hoist_declarations(copy_of(n->then_body)));
                    auto const else_body = f.add_list(hoist_declarations(copy_of(n->else_body)));
                    restate(id, flat_if{.condition = n->condition, .then_body = then_body, .else_body = else_body});
                }
                // a call with a derivative written by hand runs forward as it is, loops and all
                else if (auto const* const custom = s.node.try_as<flat_block>();
                         custom != nullptr && f.custom_calls.contains(i32(custom->label)))
                    f.block.push_back(id);
                else if (auto const* const n = s.node.try_as<flat_block>())
                    restate(id, flat_block{.label = n->label, .body = f.add_list(hoist_declarations(copy_of(n->body)))});
                else if (auto const* const loop = s.node.try_as<flat_for>();
                         loop != nullptr && constant_bounds(*loop).has_value())
                {
                    // a `continue` of this loop leaves the rest of its iteration: a block around the body
                    auto const iteration = f.add_label("iteration");
                    auto const wrapped = f.make_stmt(
                        where,
                        flat_block{.label = iteration,
                                   .body = f.add_list(continues_as_leaves(copy_of(loop->body), loop->label, iteration))});
                    ++loop_depth;
                    auto const body = f.add_list(hoist_declarations(cc::span<flat_stmt_id const>(&wrapped, 1)));
                    --loop_depth;
                    restate(id, flat_for{.label = loop->label,
                                         .index = loop->index,
                                         .first = loop->first,
                                         .end = loop->end,
                                         .body = body});
                }
                else if (s.node.is<flat_loop>() || s.node.is<flat_while>() || s.node.is<flat_for>())
                    failed("a loop whose bounds are no constants, which only a @backward can walk back");
                else if (s.node.is<flat_case>())
                    failed("a case");
                else if (s.node.is<flat_continue>() || s.node.is<flat_break>() || s.node.is<flat_discard>())
                    failed("a jump it cannot follow yet");
                else
                    f.block.push_back(id);
            }
        });
}

auto differentiator::continues_as_leaves(cc::span<flat_stmt_id const> statements, label_id loop, label_id iteration)
    -> cc::vector<flat_stmt_id>
{
    auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
    return collect(
        [&]
        {
            for (auto const id : copy)
            {
                auto const s = f.entry.at(id);
                auto const inside = [&](ast::range_of<flat_stmt_id> r)
                { return f.add_list(continues_as_leaves(copy_of(r), loop, iteration)); };
                if (auto const* const n = s.node.try_as<flat_continue>(); n != nullptr && n->target == loop)
                    restate(id, flat_leave{.target = iteration});
                else if (auto const* const n = s.node.try_as<flat_if>())
                {
                    auto const then_body = inside(n->then_body);
                    auto const else_body = inside(n->else_body);
                    restate(id, flat_if{.condition = n->condition, .then_body = then_body, .else_body = else_body});
                }
                else if (auto const* const block = s.node.try_as<flat_block>();
                         block != nullptr && !f.custom_calls.contains(i32(block->label)))
                    restate(id, flat_block{.label = block->label, .body = inside(block->body)});
                else if (auto const* const n = s.node.try_as<flat_for>())
                    restate(id, flat_for{.label = n->label,
                                         .index = n->index,
                                         .first = n->first,
                                         .end = n->end,
                                         .body = inside(n->body)});
                else
                    f.block.push_back(id);
            }
        });
}

auto differentiator::escapes(flat_stmt_id id, cc::set<i32>& out) const -> void
{
    auto const& s = f.entry.at(id);
    auto inner = cc::set<i32>();
    auto const body = [&](ast::range_of<flat_stmt_id> r)
    {
        for (auto const x : f.entry.at(r))
            escapes(x, inner);
    };
    if (auto const* const n = s.node.try_as<flat_leave>())
        out.insert(i32(n->target));
    else if (auto const* const n = s.node.try_as<flat_if>())
    {
        body(n->then_body);
        body(n->else_body);
    }
    else if (auto const* const n = s.node.try_as<flat_block>())
    {
        body(n->body);
        inner.erase(i32(n->label));
    }
    else if (auto const* const n = s.node.try_as<flat_for>())
    {
        body(n->body);
        inner.erase(i32(n->label));
    }
    for (auto const l : inner)
        out.insert(l);
}

auto differentiator::flag_of(label_id label) -> local_id
{
    if (auto const* const flag = left_flag.get_ptr(i32(label)))
        return *flag;
    auto const flag = hoist(cc::format("left_{}", f.entry.at(label).name), f.bool_type(), boolean(false));
    bookkeeping.insert(i32(flag));
    left_flag[i32(label)] = flag;
    return flag;
}

auto differentiator::constant_bounds(flat_for const& n) const -> cc::optional<loop_bounds>
{
    auto const first = f.constant_int(n.first);
    auto const end = f.constant_int(n.end);
    if (!first.has_value() || !end.has_value())
        return {};
    return loop_bounds{.first = first.value(), .count = end.value() > first.value() ? end.value() - first.value() : 0};
}

auto differentiator::tape(cc::string_view name, type_id type, flat_expr_id initial) -> local_id
{
    if (kept_loops.empty())
        return hoist(name, type, initial);
    auto size = 1;
    for (auto const& l : kept_loops)
        size *= l.count > 0 ? l.count : 1;
    if (size > k_max_tape)
    {
        failed(cc::format("loops that run {} times between them, more than a tape holds ({})", size, k_max_tape));
        return hoist(name, type, initial);
    }
    // PROTOTYPE: the flattener is always made from a checker it may change, and an array type is interned there
    auto const array = const_cast<checker&>(f.c).array_type(type, size);
    return hoist(name, array, zero(array));
}

auto differentiator::slot(local_id tape) -> flat_expr_id
{
    if (kept_loops.empty())
        return ref(tape);
    auto index = flat_expr_id::none;
    for (auto const& l : kept_loops)
    {
        auto const offset = l.first == 0 ? ref(l.index) : call("-", ref(l.index), integer(l.first));
        index = is_valid(index) ? call("+", call("*", index, integer(l.count)), offset) : offset;
    }
    auto const element = m().at(f.entry.at(tape).type).element;
    return add(element, flat_element{.object = ref(tape), .index = index});
}

auto differentiator::record(flat_expr_id condition) -> flat_expr_id
{
    auto const k = tape("taken", f.bool_type(), boolean(false));
    bookkeeping.insert(i32(k));
    emit(flat_assign{.place = slot(k), .value = condition});
    return slot(k);
}

auto differentiator::straighten(cc::span<flat_stmt_id const> statements) -> cc::vector<flat_stmt_id>
{
    auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
    return collect([&] { straighten_into(copy, 0); });
}

auto differentiator::straighten_into(cc::span<flat_stmt_id const> statements, isize from) -> void
{
    for (auto i = from; i < statements.size(); ++i)
    {
        auto const id = statements[i];
        auto const s = f.entry.at(id);
        if (auto const* const n = s.node.try_as<flat_assign>())
        {
            auto const root = root_of(n->place, nullptr);
            if (!is_valid(root) && !is_buffer_place(n->place))
                return (void)failed("an assignment to a part of a local it cannot follow yet");
            if (is_valid(root) && !single_assigned.contains(i32(root)) && !bookkeeping.contains(i32(root)))
            {
                auto const save = tape("saved", type_of(n->place), zero(type_of(n->place)));
                bookkeeping.insert(i32(save));
                emit(flat_assign{.place = slot(save), .value = clone(n->place)});
                save_of[i32(id)] = slot(save);
            }
            f.block.push_back(id);
        }
        else if (auto const* const n = s.node.try_as<flat_leave>())
            emit(flat_assign{.place = ref(flag_of(n->target)), .value = boolean(true)});
        else if (auto const* const n = s.node.try_as<flat_if>())
        {
            auto const condition = record(n->condition);
            auto const then_body = f.add_list(straighten(copy_of(n->then_body)));
            auto const else_body = f.add_list(straighten(copy_of(n->else_body)));
            restate(id, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
        }
        else if (auto const* const block = s.node.try_as<flat_block>();
                 block != nullptr && !f.custom_calls.contains(i32(block->label)))
        {
            auto body = straighten(copy_of(block->body));
            // a block in a loop runs again, and its leave of the last run must not guard this one
            if (auto const* const flag = left_flag.get_ptr(i32(block->label)))
                body.insert_at(0, f.make_stmt(where, flat_assign{.place = ref(*flag), .value = boolean(false)}));
            restate(id, flat_block{.label = block->label, .body = f.add_list(body)});
        }
        else if (auto const* const n = s.node.try_as<flat_for>())
        {
            auto const bounds = constant_bounds(*n).value();
            auto left = cc::set<i32>();
            for (auto const x : f.entry.at(n->body))
                escapes(x, left);
            // a break leaves to the loop, whose flag starts false each time the loop does
            if (left.contains(i32(n->label)))
                emit(flat_assign{.place = ref(flag_of(n->label)), .value = boolean(false)});
            kept_loops.push_back({.index = n->index, .first = bounds.first, .count = bounds.count});
            auto body = straighten(copy_of(n->body));
            if (!left.empty())
                body = collect(
                    [&]
                    {
                        auto still = flat_expr_id::none;
                        for (auto const l : left)
                        {
                            auto const stays = add(f.bool_type(), flat_not{.operand = ref(flag_of(label_id(l)))});
                            still = is_valid(still) ? add(f.bool_type(), flat_and{.lhs = still, .rhs = stays}) : stays;
                        }
                        auto const runs = record(still);
                        emit(flat_if{.condition = runs,
                                     .then_body = f.add_list(body),
                                     .else_body = f.add_list(cc::span<flat_stmt_id const>())});
                    });
            kept_loops.remove_back();
            restate(
                id,
                flat_for{.label = n->label, .index = n->index, .first = n->first, .end = n->end, .body = f.add_list(body)});
        }
        else
            f.block.push_back(id);

        auto escaped = cc::set<i32>();
        escapes(id, escaped);
        if (escaped.empty() || i + 1 == statements.size())
            continue;
        // what follows runs only where no leave of this statement was taken
        auto still = flat_expr_id::none;
        for (auto const l : escaped)
        {
            auto const stays = add(f.bool_type(), flat_not{.operand = ref(flag_of(label_id(l)))});
            still = is_valid(still) ? add(f.bool_type(), flat_and{.lhs = still, .rhs = stays}) : stays;
        }
        auto const condition = record(still);
        auto const rest = collect([&] { straighten_into(statements, i + 1); });
        emit(flat_if{.condition = condition,
                     .then_body = f.add_list(rest),
                     .else_body = f.add_list(cc::span<flat_stmt_id const>())});
        return;
    }
}

} // namespace sgl::check::impl
