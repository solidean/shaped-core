#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/differentiator.hh>

// PROTOTYPE: Reverse mode's backward sweep: each statement's tangent transposed, last first, and the derivatives written by hand.

namespace sgl::check::impl
{
auto differentiator::adjoint_of_local(local_id local, flat_expr_id initial) -> local_id
{
    if (auto const* const a = adjoint_of.get_ptr(i32(local)))
        return *a;
    auto const type = linear_locals.contains(i32(local)) ? f.entry.at(local).type : tangent(f.entry.at(local).type);
    auto const a = f.add_local(local_kind::var, cc::format("a_{}", f.entry.at(local).name), type);
    adjoint_declarations.push_back(
        f.make_stmt(where, flat_var{.local = a, .value = is_valid(initial) ? initial : zero(type)}));
    adjoint_of[i32(local)] = a;
    return a;
}

auto differentiator::is_linear(flat_expr_id id) -> bool
{
    if (!is_valid(id))
        return false;
    if (auto const* const known = linear_memo.get_ptr(i32(id)))
        return *known;
    auto const x = node(id);
    auto result = false;
    if (auto const* const r = x.node.try_as<flat_local_ref>())
        result = linear_locals.contains(i32(r->local));
    else if (auto const* const n = x.node.try_as<flat_member>())
        result = is_linear(n->object);
    else if (auto const* const n = x.node.try_as<flat_element>())
        result = is_linear(n->object);
    else if (auto const* const n = x.node.try_as<flat_construct>())
        for (auto const a : copy_of(n->arguments))
            result = result || is_linear(a);
    else if (auto const* const n = x.node.try_as<flat_call>())
        for (auto const a : copy_of(n->arguments))
            result = result || is_linear(a);
    linear_memo[i32(id)] = result;
    return result;
}

auto differentiator::adjoint_target(local_id leaf) const -> local_id
{
    auto const* const primal = primal_of_placeholder.get_ptr(i32(leaf));
    return primal != nullptr ? *primal : leaf;
}

auto differentiator::sum_of_components(flat_expr_id g) -> flat_expr_id
{
    auto const t = type_of(g);
    auto result = flat_expr_id::none;
    for (auto i = 0; i < leaf_count(t); ++i)
        result = sum(result, add(f.field_type(t, i), flat_member{.object = use(g), .member = i}));
    return result;
}

auto differentiator::inner_product(flat_expr_id a, flat_expr_id b) -> flat_expr_id
{
    auto const column_count = leaf_count(type_of(a)) / leaf_count(type_of(call("matrix_column", use(a), integer(0))));
    auto result = flat_expr_id::none;
    for (auto c = 0; c < column_count; ++c)
        result = sum(result,
                     call("dot", call("matrix_column", use(a), integer(c)), call("matrix_column", use(b), integer(c))));
    return result;
}

auto differentiator::reduce(flat_expr_id g, type_id to) -> flat_expr_id
{
    if (type_of(g) == to || !is_scalar(to) || !is_vector(type_of(g)))
        return use(g);
    return sum_of_components(g);
}

auto differentiator::component(flat_expr_id g, i32 k) -> flat_expr_id
{
    auto const t = type_of(g);
    if (is_scalar(t))
        return use(g);
    if (is_vector(t))
        return add(f.field_type(t, k), flat_member{.object = use(g), .member = k});
    auto const column = call("matrix_column", use(g), integer(0));
    auto const rows = leaf_count(type_of(column));
    auto const c = call("matrix_column", use(g), integer(k / rows));
    return add(f.field_type(type_of(c), k % rows), flat_member{.object = c, .member = k % rows});
}

auto differentiator::assemble(type_id t, cc::span<flat_expr_id const> parts) -> flat_expr_id
{
    if (is_scalar(t))
        return parts[0];
    if (is_vector(t))
        return add(t, flat_construct{.arguments = f.add_list(parts)});
    auto const* const overloads = f.c.prelude_names.get_ptr(cc::string(m().name_of(t)));
    if (overloads == nullptr)
        return failed(cc::format("no way to build a {}", m().name_of(t)));
    for (auto const callee : *overloads)
    {
        auto const& s = m().at(callee);
        if (s.kind != symbol_kind::function || s.info < 0)
            continue;
        auto const& info = m().functions[s.info];
        auto const parameters = m().at(info.parameters);
        if (info.result != t || isize(parameters.size() * parameters.size()) != parts.size())
            continue;
        auto columns = cc::vector<flat_expr_id>();
        auto const rows = parameters.size();
        for (auto c = isize(0); c < rows; ++c)
            columns.push_back(assemble(parameters[c].type, parts.subspan({.offset = c * rows, .size = rows})));
        return f.builtin_call(at, callee, columns);
    }
    return failed(cc::format("no way to build a {}", m().name_of(t)));
}

auto differentiator::one_hot(type_id t, flat_member const& member, flat_expr_id g) -> flat_expr_id
{
    auto const& info = m().at(t);
    auto parts = cc::vector<flat_expr_id>();
    for (auto j = 0; j < i32(info.members.count); ++j)
    {
        auto part = flat_expr_id::none;
        if (!member.is_swizzle())
            part = j == member.member ? use(g) : flat_expr_id::none;
        else
            for (auto k = 0; k < member.letters.count; ++k)
                if (member.letters.fields[k] == j)
                    part = sum(part, add(f.field_type(t, j), flat_member{.object = use(g), .member = k}));
        parts.push_back(is_valid(part) ? part : zero(f.field_type(t, j)));
    }
    return add(t, flat_construct{.arguments = f.add_list(parts)});
}

auto differentiator::transpose(flat_expr_id t, flat_expr_id g) -> void
{
    if (!is_valid(g) || !is_linear(t) || is_failed)
        return;
    auto const x = node(t);
    // a field or an element of a leaf: added where it stands
    {
        auto steps = cc::vector<place_step>();
        auto const root = root_of(t, &steps);
        if (is_valid(root) && linear_locals.contains(i32(root)))
        {
            auto const target = adjoint_of_local(adjoint_target(root));
            return accumulate([&] { return ref(target); }, cc::move(steps), g);
        }
    }
    if (auto const* const n = x.node.try_as<flat_member>())
        return transpose(n->object, share(one_hot(type_of(n->object), *n, g)));
    if (auto const* const n = x.node.try_as<flat_construct>())
    {
        auto const arguments = copy_of(n->arguments);
        auto const is_array = m().at(x.type).kind == type_kind::array;
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (!is_linear(arguments[i]))
                continue;
            auto const part = is_array
                                ? add(type_of(arguments[i]), flat_element{.object = use(g), .index = integer(i32(i))})
                                : add(type_of(arguments[i]), flat_member{.object = use(g), .member = i32(i)});
            transpose(arguments[i], share(part));
        }
        return;
    }
    auto const* const n = x.node.try_as<flat_call>();
    if (n == nullptr)
        return (void)failed("a tangent it cannot transpose");
    auto const name = rule_name(n->callee);
    auto const arguments = copy_of(n->arguments);
    auto const linear_at = [&](isize i) { return i < arguments.size() && is_linear(arguments[i]); };
    auto const r = x.type;

    if ((name == "+" || name == "-") && arguments.size() == 1)
        return transpose(arguments[0], name == "+" ? g : share(negate(use(g))));
    if ((name == "+" || name == "-") && arguments.size() == 2)
    {
        if (linear_at(0))
            transpose(arguments[0], share(reduce(g, type_of(arguments[0]))));
        if (linear_at(1))
        {
            auto const negated = name == "-" ? share(negate(use(g))) : g;
            transpose(arguments[1], share(reduce(negated, type_of(arguments[1]))));
        }
        return;
    }
    if (name == "*" && arguments.size() == 2)
    {
        auto const k = linear_at(0) ? 0 : 1;
        auto const u = arguments[k];
        auto const w = share(arguments[1 - k], "w");
        auto const ut = type_of(u);
        auto const wt = type_of(w);
        auto gu = flat_expr_id::none;
        if (!is_matrix(ut) && !is_matrix(wt) && (ut == r || is_scalar(wt)))
            gu = mul(use(g), use(w));
        else if (is_scalar(ut) && is_vector(wt))
        {
            type_id const both[] = {r, wt};
            gu = is_valid(find("dot", both)) ? call("dot", use(g), use(w))
                                             : sum_of_components(share(mul(use(g), use(w))));
        }
        else if (is_matrix(ut) && k == 0 && is_vector(wt))
        {
            // the outer product of the result's adjoint and the vector
            auto parts = cc::vector<flat_expr_id>();
            for (auto c = 0; c < leaf_count(wt); ++c)
                for (auto row = 0; row < leaf_count(r); ++row)
                    parts.push_back(mul(component(g, row), component(w, c)));
            if (isize(leaf_count(ut)) != parts.size())
                return (void)failed(cc::format("the adjoint of {} * {}", m().name_of(ut), m().name_of(wt)));
            gu = assemble(ut, parts);
        }
        else if (is_vector(ut) && k == 1 && is_matrix(wt))
            gu = call("*", call("transpose", use(w)), use(g));
        else if (is_matrix(ut) && is_scalar(wt))
            gu = mul(use(g), use(w));
        else if (is_scalar(ut) && is_matrix(wt))
            gu = inner_product(g, w);
        else if (is_matrix(ut) && is_matrix(wt))
            gu = k == 0 ? call("*", use(g), call("transpose", use(w))) : call("*", call("transpose", use(w)), use(g));
        else
            return (void)failed(cc::format("the adjoint of {} * {}", m().name_of(type_of(arguments[0])),
                                           m().name_of(type_of(arguments[1]))));
        return transpose(u, share(gu));
    }
    if (name == "/" && linear_at(0))
        return transpose(arguments[0], share(reduce(share(call("/", use(g), arguments[1])), type_of(arguments[0]))));
    if (name == "dot" && arguments.size() == 2)
    {
        auto const k = linear_at(0) ? 0 : 1;
        return transpose(arguments[k], share(mul(arguments[1 - k], use(g))));
    }
    if (name == "cross" && arguments.size() == 2)
    {
        auto const k = linear_at(0) ? 0 : 1;
        auto const w = arguments[1 - k];
        return transpose(arguments[k], share(k == 0 ? call("cross", w, use(g)) : call("cross", use(g), w)));
    }
    if (name == "transpose")
        return transpose(arguments[0], share(call("transpose", use(g))));
    if (name == "select" && arguments.size() == 3)
    {
        auto const condition = share(arguments[0]);
        if (linear_at(1))
            transpose(arguments[1], share(call("select", use(condition), use(g), zero(r))));
        if (linear_at(2))
            transpose(arguments[2], share(call("select", use(condition), zero(r), use(g))));
        return;
    }
    if (name == "matrix_column" && arguments.size() == 2)
    {
        // the matrix whose column `i` is `g`, and every other column zero
        auto const i = share(arguments[1]);
        auto const ut = type_of(arguments[0]);
        auto const columns = leaf_count(ut) / leaf_count(r);
        auto parts = cc::vector<flat_expr_id>();
        for (auto c = 0; c < columns; ++c)
        {
            auto const is_this = call("==", use(i), integer(c));
            auto const column = share(call("select", is_this, use(g), zero(r)));
            for (auto row = 0; row < leaf_count(r); ++row)
                parts.push_back(component(column, row));
        }
        return transpose(arguments[0], share(assemble(ut, parts)));
    }
    if (is_linear_function(name, arguments.size()) && leaf_count(r) > 0)
    {
        // a construction or a conversion: each argument's adjoint is the components of `g` it filled
        auto total = 0;
        for (auto const a : arguments)
            total += leaf_count(type_of(a));
        auto offset = 0;
        for (auto const a : arguments)
        {
            auto const n_a = leaf_count(type_of(a));
            if (n_a < 0)
                return (void)failed(cc::format("the adjoint of {}", name));
            if (is_linear(a))
            {
                if (record_of(type_of(a))->leaf_kind != record_of(r)->leaf_kind)
                    return (void)failed(cc::format("the adjoint of {} between float widths", name));
                auto parts = cc::vector<flat_expr_id>();
                if (total == leaf_count(r))
                    for (auto k = 0; k < n_a; ++k)
                        parts.push_back(component(g, offset + k));
                else if (arguments.size() == 1 && n_a == 1)
                    parts.push_back(sum_of_components(g));
                else
                    return (void)failed(cc::format("the adjoint of {}", name));
                transpose(a, share(assemble(type_of(a), parts)));
            }
            offset += n_a;
        }
        return;
    }
    failed(cc::format("no adjoint of {} yet", name));
}

auto differentiator::backward(cc::span<flat_stmt_id const> statements) -> void
{
    auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
    for (auto i = copy.size(); i-- > 0;)
        backward_stmt(copy[i]);
}

auto differentiator::backward_stmt(flat_stmt_id id) -> void
{
    auto const s = f.entry.at(id);
    if (auto const* const n = s.node.try_as<flat_assign>())
    {
        auto steps = cc::vector<place_step>();
        auto const root = root_of(clone(n->place), &steps);
        if (!is_valid(root) || bookkeeping.contains(i32(root)))
            return;
        if (auto const* const save = save_of.get_ptr(i32(id)))
            emit(flat_assign{.place = clone(n->place), .value = clone(*save)});
        if (!tracked.contains(i32(root)) || !is_differentiable(type_of(n->place)))
            return;
        auto const adjoint = adjoint_of_local(root);
        auto const g = share(place_of(ref(adjoint), steps), "g");
        emit(flat_assign{.place = place_of(ref(adjoint), steps), .value = zero(tangent(type_of(n->place)))});
        linear_lets.clear();
        gradient_reads.clear();
        auto const t = tangent_of_value(n->value);
        transpose(t, g);
        // a tangent read twice was bound once; its adjoint is complete only once every read is transposed
        for (auto k = linear_lets.size(); k-- > 0;)
        {
            auto const l = linear_lets[k];
            if (auto const* const a = adjoint_of.get_ptr(i32(l)))
            {
                auto const adjoint = *a;
                transpose(linear_definition.get(i32(l)), ref(adjoint));
                // a statement in a kept loop runs again, and this adjoint is its alone
                emit(flat_assign{.place = ref(adjoint), .value = zero(f.entry.at(adjoint).type)});
            }
        }
        add_gradient_reads();
        return;
    }
    if (auto const* const n = s.node.try_as<flat_if>())
    {
        auto const then_body = collect([&] { backward(copy_of(n->then_body)); });
        auto const else_body = collect([&] { backward(copy_of(n->else_body)); });
        return emit(flat_if{.condition = clone(n->condition),
                            .then_body = f.add_list(then_body),
                            .else_body = f.add_list(else_body)});
    }
    // a kept loop is walked back from its last iteration, its index read reversed by every clone of its body
    if (auto const* const n = s.node.try_as<flat_for>())
    {
        auto const bounds = constant_bounds(*n).value();
        auto const back = f.add_local(local_kind::index, "back", f.int_type());
        auto const index = n->index;
        auto const body = copy_of(n->body);
        auto const reversed_body = collect(
            [&]
            {
                auto const reversed = f.add_local(local_kind::let, f.entry.at(index).name, f.int_type());
                emit(flat_let{.local = reversed, .value = call("-", integer(bounds.first + bounds.count - 1), ref(back))});
                substitution[i32(index)] = reversed;
                backward(body);
                substitution.erase(i32(index));
            });
        return emit(flat_for{.label = f.add_label("back"),
                             .index = back,
                             .first = integer(0),
                             .end = integer(bounds.count),
                             .body = f.add_list(reversed_body)});
    }
    if (auto const* const n = s.node.try_as<flat_block>())
    {
        if (auto const* const custom = f.custom_calls.get_ptr(i32(n->label)))
            return backward_custom(*custom, n->label);
        auto const body = collect([&] { backward(copy_of(n->body)); });
        emit(flat_block{.label = f.add_label("back"), .body = f.add_list(body)});
    }
}

auto differentiator::parameters_of(symbol_id function) const -> cc::span<parameter const>
{
    return m().at(m().functions[m().at(function).info].parameters);
}

auto differentiator::fits(flattener::custom_call const& custom, symbol_id derivative, type_id result) const -> bool
{
    auto const own = parameters_of(custom.function);
    auto const theirs = parameters_of(derivative);
    auto k = isize(0);
    for (auto i = isize(0); i < own.size(); ++i)
    {
        if (k >= theirs.size() || theirs[k++].type != own[i].type)
            return false;
        if (!f.c.takes_tangent(custom.function, i))
            continue;
        if (k >= theirs.size() || theirs[k].type != tangent(own[i].type) || theirs[k].is_mut != is_reverse)
            return false;
        ++k;
    }
    auto const& info = m().functions[m().at(derivative).info];
    if (is_reverse)
        return k + 1 == theirs.size() && theirs[k].type == tangent(result) && info.result == checked_module::void_type;
    return k == theirs.size() && info.result == tangent(result);
}

auto differentiator::inline_derivative(flattener::custom_call const& custom,
                                       symbol_id derivative,
                                       cc::span<flat_expr_id const> arguments) -> flat_expr_id
{
    auto const result = m().functions[m().at(derivative).info].result;
    auto const inlined = f.inline_call(custom.call, derivative, arguments);
    if (!is_valid(inlined.label))
        return failed(cc::format("{}, which did not inline", m().at(derivative).name));
    return add(result, flat_block{.label = inlined.label, .body = inlined.body});
}

auto differentiator::forward_custom(flat_stmt_id id, flattener::custom_call const& custom, label_id label) -> void
{
    f.block.push_back(id);
    // `no_diff(x)`: the value as it is, and its tangent zero
    if (custom.is_detached)
        return;
    auto const* const result = lifted.get_ptr(i32(label));
    auto const* const result_tangent = result != nullptr ? tangent_of.get_ptr(i32(*result)) : nullptr;
    if (result_tangent == nullptr)
        return;
    auto const tangent_local = *result_tangent;
    auto const result_type = f.entry.at(*result).type;
    if (!is_valid(custom.derivatives.forward))
        return (void)failed(cc::format("{} has a @backward and no @forward", m().at(custom.function).name));
    if (!fits(custom, custom.derivatives.forward, result_type))
        return (void)failed(cc::format("@forward({}) takes each parameter and its tangent, and gives the result",
                                       m().at(custom.function).name));
    auto arguments = cc::vector<flat_expr_id>();
    for (auto i = isize(0); i < custom.arguments.size(); ++i)
    {
        auto const a = custom.arguments[i];
        arguments.push_back(ref(a));
        if (!f.c.takes_tangent(custom.function, i))
            continue;
        auto const* const t = tangent_of.get_ptr(i32(a));
        arguments.push_back(t != nullptr ? ref(*t) : zero(tangent(f.entry.at(a).type)));
    }
    emit(flat_assign{.place = ref(tangent_local),
                     .value = inline_derivative(custom, custom.derivatives.forward, arguments)});
}

auto differentiator::backward_custom(flattener::custom_call const& custom, label_id label) -> void
{
    auto const* const result = lifted.get_ptr(i32(label));
    if (custom.is_detached || result == nullptr || !tracked.contains(i32(*result)))
        return;
    auto const result_local = *result;
    auto const result_type = f.entry.at(result_local).type;
    if (!is_valid(custom.derivatives.backward))
        return (void)failed(cc::format("{} has a @forward and no @backward", m().at(custom.function).name));
    if (!fits(custom, custom.derivatives.backward, result_type))
        return (void)failed(cc::format("@backward({}) takes each parameter and its adjoint as `mut`, then the "
                                       "result's adjoint",
                                       m().at(custom.function).name));
    auto const adjoint = adjoint_of_local(result_local);
    auto const g = share(ref(adjoint), "g");
    emit(flat_assign{.place = ref(adjoint), .value = zero(tangent(result_type))});
    auto arguments = cc::vector<flat_expr_id>();
    for (auto i = isize(0); i < custom.arguments.size(); ++i)
    {
        auto const a = custom.arguments[i];
        arguments.push_back(ref(a));
        if (!f.c.takes_tangent(custom.function, i))
            continue;
        // what is not followed takes its adjoint into a local nobody reads
        auto const place = tracked.contains(i32(a))
                             ? adjoint_of_local(a)
                             : f.add_local(local_kind::var, "ignored", tangent(f.entry.at(a).type));
        if (!tracked.contains(i32(a)))
            emit(flat_var{.local = place, .value = zero(tangent(f.entry.at(a).type))});
        arguments.push_back(ref(place));
    }
    arguments.push_back(use(g));
    auto const call = inline_derivative(custom, custom.derivatives.backward, arguments);
    if (is_valid(call))
        emit(flat_block{.label = node(call).node.as<flat_block>().label, .body = node(call).node.as<flat_block>().body});
}

auto differentiator::atomic_add_of(type_id element) const -> symbol_id
{
    // the builtin's atomic is a `mut atomic[T]` of its own, so it is matched by what the atomic holds
    auto const* const found = f.c.prelude_names.get_ptr(cc::string("add"));
    if (found == nullptr)
        return symbol_id::none;
    for (auto const candidate : *found)
    {
        auto const& s = m().at(candidate);
        if (s.kind != symbol_kind::function || s.info < 0)
            continue;
        auto const parameters = m().at(m().functions[s.info].parameters);
        if (parameters.size() == 2 && m().at(parameters[0].type).kind == type_kind::atomic
            && m().at(parameters[0].type).element == element && parameters[1].type == element)
            return candidate;
    }
    return symbol_id::none;
}

auto differentiator::add_gradient_reads() -> void
{
    for (auto const& r : gradient_reads)
    {
        auto const* const a = adjoint_of.get_ptr(i32(r.placeholder));
        if (a == nullptr)
            continue;
        auto const adjoint = *a;
        auto const buffer
            = add(r.gradients.type, flat_binding_member{.binding = r.gradients.binding, .member = r.gradients.member});
        auto const atomic = m().at(r.gradients.type).element;
        auto const place = add(atomic, flat_buffer_element{.buffer = buffer, .index = use(r.index)});
        auto const callee = atomic_add_of(m().at(atomic).element);
        if (!is_valid(callee))
            return (void)failed(cc::format("an atomic add of {}, which no builtin is", m().name_of(atomic)));
        flat_expr_id const arguments[] = {place, ref(adjoint)};
        emit(flat_eval{.value = f.builtin_call(at, callee, arguments)});
        // a read in a kept loop is transposed once per iteration, each adjoint its own
        emit(flat_assign{.place = ref(adjoint), .value = zero(f.entry.at(adjoint).type)});
    }
    gradient_reads.clear();
}

} // namespace sgl::check::impl
