#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/differentiator.hh>

// PROTOTYPE of libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md.
// `fwd_diff` and `bwd_diff`, written as a transform of the flat tree.
//
// Both modes start alike.
// The function is inlined over locals that hold its arguments, and the body is LOWERED (flatten_diff_lower.cc).
// Every block that stands in an expression is lifted into a statement block that assigns a local, so no statement's value holds a block.
// `derive` then writes the tangent of a value as linear operations over primal values (flatten_diff_forward.cc).
//
// Forward mode writes the tangent of every statement beside it.
// Reverse mode first hoists every declaration, turns every early leave into a flag and records every condition (flatten_diff_straighten.cc).
// A loop of constant bounds stays a loop, and what it saves or records is a tape with a slot per iteration.
// Its backward sweep walks that code in reverse (flatten_diff_backward.cc): it restores each value a statement overwrote.
// It then TRANSPOSES the statement's tangent, which pushes the result's adjoint onto the adjoints of what the statement read.
//
// Every node a pass writes is attributed to the call of `fwd_diff` or `bwd_diff`.
// A node is placed in the tree at most once: a value read twice is `share`d, and each read is a `use` of it.

auto sgl::check::impl::flattener::custom_derivatives_of(symbol_id function) const -> custom_derivatives
{
    // PROTOTYPE: matched by name, since an attribute's argument is no expression the check pass resolves yet
    auto result = custom_derivatives();
    auto const& target = c.out.at(function);
    for (auto i = isize(0); i < c.out.symbols.size(); ++i)
    {
        auto const& s = c.out.symbols[i];
        if (s.kind != symbol_kind::function || !ast::is_valid(s.declaration))
            continue;
        auto const& declaration = c.ast_of(s.file).at(s.declaration);
        if (!declaration.node.is<ast::fun_decl>())
            continue;
        for (auto const which : {cc::string_view("forward"), cc::string_view("backward")})
        {
            auto const* const a = c.find_attribute(s.file, declaration.attributes, which);
            if (a == nullptr)
                continue;
            auto const arguments = c.ast_of(s.file).at(a->arguments);
            if (arguments.size() != 1 || !ast::is_valid(arguments[0].value)
                || c.text_of(s.file, c.span_of(s.file, arguments[0].value)) != target.name)
                continue;
            (which == "forward" ? result.forward : result.backward) = symbol_id(i);
        }
    }
    return result;
}

auto sgl::check::impl::flattener::flatten_custom_call(ast::expr_id id,
                                                      type_id type,
                                                      call_record const& record,
                                                      custom_derivatives d,
                                                      cc::span<flat_expr_id const> values) -> flat_expr_id
{
    auto const slots = c.out.at(record.slots);
    auto const parameters = c.out.at(c.out.functions[c.out.at(record.callee).info].parameters);
    auto arguments = cc::vector<local_id>::create_filled(parameters.size(), local_id::none);
    // each argument in a local of its own, evaluated in the order written, which the derivative then reads
    for (auto k = isize(0); k < values.size(); ++k)
    {
        if (slots[k] < 0 || slots[k] >= parameters.size() || !is_valid(values[k]) || parameters[slots[k]].is_mut)
            return fail();
        auto const local = add_local(local_kind::let, parameters[slots[k]].name, entry.at(values[k]).type);
        add_stmt({.file = file(), .expr = id}, flat_let{.local = local, .value = values[k]});
        arguments[slots[k]] = local;
    }
    auto references = cc::vector<flat_expr_id>();
    for (auto const a : arguments)
    {
        if (!is_valid(a))
            return fail();
        references.push_back(local_ref(a, id));
    }
    auto const inlined = inline_call(id, record.callee, references);
    if (!is_valid(inlined.label))
        return fail();
    custom_calls[i32(inlined.label)]
        = {.function = record.callee, .derivatives = d, .arguments = cc::move(arguments), .call = id};
    return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
}

auto sgl::check::impl::flattener::is_detach_call(symbol_id callee) const -> bool
{
    auto const& s = c.out.at(callee);
    return s.name == "no_diff" && c.is_prelude_file(s.file);
}

auto sgl::check::impl::flattener::is_diff_call(symbol_id callee) const -> bool
{
    auto const& s = c.out.at(callee);
    return (s.name == "fwd_diff" || s.name == "bwd_diff") && c.is_prelude_file(s.file);
}

auto sgl::check::impl::flattener::flatten_diff(ast::expr_id id, type_id type, call_record const& record) -> flat_expr_id
{
    auto d = differentiator{.f = *this, .is_reverse = c.out.at(record.callee).name == "bwd_diff", .at = id};
    d.where = origin{.file = file(), .expr = id};

    auto const values = flatten_written(c.out.at(record.written));
    auto const handed = written_closures;
    auto const slots = c.out.at(record.slots);
    auto const* const signature = c.derivative_signatures.get_ptr(i32(record.callee));
    if (signature == nullptr)
        return d.failed("a lambda or a parameter handed on, where it can differentiate a function's name alone");
    auto const arity = signature->value_slots.size();
    auto function_slot = isize(-1);
    for (auto k = isize(0); k < slots.size(); ++k)
        if (slots[k] == 0)
            function_slot = k;
    if (function_slot < 0 || function_slot >= handed.size() || handed[function_slot] < 0)
        return d.failed("the function to differentiate is no function it can inline");

    auto const label = add_label(d.is_reverse ? "bwd_diff" : "fwd_diff");
    auto outer = cc::move(block);
    block = {};
    auto const give_up = [&](cc::string_view why)
    {
        block = cc::move(outer);
        return d.failed(why);
    };

    // every argument is evaluated once, in the order written, before anything else
    auto primals = cc::vector<local_id>();
    auto seconds = cc::vector<flat_expr_id>();
    primals.resize_to_filled(arity, local_id::none);
    seconds.resize_to_filled(arity, flat_expr_id::none);
    auto result_adjoint = flat_expr_id::none;
    for (auto k = isize(0); k < values.size(); ++k)
    {
        auto const p = slots[k];
        if (p <= 0 || !is_valid(values[k]))
            continue;
        if (p == signature->result_slot)
        {
            result_adjoint = d.share(values[k], "dr");
            continue;
        }
        for (auto i = isize(0); i < arity; ++i)
        {
            if (p == signature->value_slots[i])
            {
                auto const local = add_local(local_kind::let, "x", entry.at(values[k]).type);
                add_stmt(d.where, flat_let{.local = local, .value = values[k]});
                primals[i] = local;
            }
            else if (p == signature->tangent_slots[i])
                seconds[i] = d.is_reverse ? pin_place(values[k]) : d.share(values[k], "dx");
        }
    }
    for (auto const p : primals)
        if (!is_valid(p))
            return give_up("an argument it could not bind");

    // the function, inlined over the locals
    auto references = cc::vector<flat_expr_id>();
    auto identity = cc::vector<i32>();
    auto no_closures = cc::vector<i32>();
    for (auto i = isize(0); i < arity; ++i)
    {
        references.push_back(local_ref(primals[i], id));
        identity.push_back(i32(i));
        no_closures.push_back(-1);
    }
    auto const result_type = signature->result;
    ++diff_depth;
    // a generic's instance, by the bindings the check deduced
    auto const inlined = signature->bindings.empty()
                           ? call_closure(id, result_type, handed[function_slot], references, identity, no_closures)
                           : [&]
    {
        auto const body = inline_bound(id, signature->function, references, identity, no_closures, signature->bindings);
        return is_valid(body.label) ? add_expr(result_type, id, flat_block{.label = body.label, .body = body.body})
                                    : flat_expr_id::none;
    }();
    --diff_depth;
    if (!is_valid(inlined))
        return give_up("the function, which did not inline");
    auto const result = add_local(local_kind::let, "result", result_type);
    auto const unlowered = make_stmt(d.where, flat_let{.local = result, .value = inlined});
    auto const seeds = cc::move(block);
    block = {};
    auto const body = d.collect([&] { d.lower_list(cc::span<flat_stmt_id const>(&unlowered, 1)); });

    auto statements = seeds;
    if (!d.is_reverse)
    {
        for (auto i = isize(0); i < arity; ++i)
            if (d.is_differentiable(entry.at(primals[i]).type) && is_valid(seconds[i]))
            {
                auto const t = add_local(local_kind::let, "dx", entry.at(seconds[i]).type);
                statements.push_back(make_stmt(d.where, flat_let{.local = t, .value = d.use(seconds[i])}));
                d.tangent_of[i32(primals[i])] = t;
            }
        for (auto const s : d.forward(body))
            statements.push_back(s);
        auto const* const t = d.tangent_of.get_ptr(i32(result));
        auto const tangent = t != nullptr ? local_ref(*t, id) : d.zero(type);
        statements.push_back(make_stmt(d.where, flat_leave{.target = label, .value = tangent}));
    }
    else
    {
        for (auto i = isize(0); i < arity; ++i)
            if (d.is_differentiable(entry.at(primals[i]).type) && is_valid(seconds[i]))
                d.tracked.insert(i32(primals[i]));
        auto const flat = d.hoist_declarations(body);
        auto const straight = d.straighten(flat);
        d.adjoint_of_local(result, d.use(result_adjoint));
        auto const sweep = d.collect([&] { d.backward(straight); });
        auto const outputs = d.collect(
            [&]
            {
                for (auto i = isize(0); i < arity; ++i)
                {
                    auto const* const a = d.adjoint_of.get_ptr(i32(primals[i]));
                    if (a == nullptr || !is_valid(seconds[i]))
                        continue;
                    auto const adjoint = *a;
                    auto const place = seconds[i];
                    d.accumulate([&] { return again(place, id); }, {}, d.share(local_ref(adjoint, id)));
                }
            });
        cc::vector<flat_stmt_id> const* const parts[]
            = {&d.hoisted, &straight, &d.adjoint_declarations, &sweep, &outputs};
        for (auto const* const part : parts)
            for (auto const s : *part)
                statements.push_back(s);
    }
    block = cc::move(outer);
    if (d.is_failed)
        return flat_expr_id::none;
    return add_expr(type, id, flat_block{.label = label, .body = add_list(statements)});
}
