#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/differentiator.hh>

// PROTOTYPE: The differentiator's tangents: each builtin's rule, the places a derivative writes, and forward mode.

namespace sgl::check::impl
{
auto differentiator::tangent_leaf(local_id local) -> flat_expr_id
{
    if (!is_reverse)
    {
        auto const* const t = tangent_of.get_ptr(i32(local));
        return t != nullptr ? ref(*t) : flat_expr_id::none;
    }
    if (!tracked.contains(i32(local)))
        return flat_expr_id::none;
    if (auto const* const p = placeholder_of.get_ptr(i32(local)))
        return ref(*p);
    auto const p = f.add_local(local_kind::temporary, cc::format("t_{}", f.entry.at(local).name),
                               tangent(f.entry.at(local).type));
    placeholder_of[i32(local)] = p;
    primal_of_placeholder[i32(p)] = local;
    linear_locals.insert(i32(p));
    return ref(p);
}

auto differentiator::derive(flat_expr_id id) -> dual
{
    if (!is_valid(id) || is_failed)
        return {id, flat_expr_id::none};
    auto const x = node(id);
    if (!is_differentiable(x.type))
        return {id, flat_expr_id::none};
    if (auto const* const r = x.node.try_as<flat_local_ref>())
        return {id, tangent_leaf(r->local)};
    if (x.node.is<flat_literal>())
        return {id, flat_expr_id::none};
    // a constant block's member is a constant: a camera, a setting, what the host hands every invocation alike
    if (auto const* const member = x.node.try_as<flat_binding_member>(); member != nullptr && !member->is_workgroup)
        return {id, flat_expr_id::none};
    if (auto const* const n = x.node.try_as<flat_buffer_element>())
        return derive_buffer_read(id, x.type, *n);
    // a buffer holds what a gradient would have to be written back into, which a derivative never does silently
    if (x.node.is<flat_binding_member>())
    {
        failed("a read of a buffer, whose gradient it cannot write back; read it as `no_diff(…)` to hold it "
               "constant, or give the buffer a @gradient");
        return {id, flat_expr_id::none};
    }
    if (auto const* const n = x.node.try_as<flat_member>())
    {
        auto const object = derive(n->object);
        auto const primal = add(x.type, flat_member{.object = object.primal, .member = n->member, .letters = n->letters});
        if (!is_valid(object.tangent))
            return {primal, flat_expr_id::none};
        return {primal,
                add(tangent(x.type), flat_member{.object = object.tangent, .member = n->member, .letters = n->letters})};
    }
    if (auto const* const n = x.node.try_as<flat_element>())
    {
        auto const object = derive(n->object);
        auto const index = share(n->index, "index");
        auto const primal = add(x.type, flat_element{.object = object.primal, .index = use(index)});
        if (!is_valid(object.tangent))
            return {primal, flat_expr_id::none};
        return {primal, add(tangent(x.type), flat_element{.object = object.tangent, .index = use(index)})};
    }
    if (auto const* const n = x.node.try_as<flat_construct>())
    {
        auto const arguments = copy_of(n->arguments);
        auto primals = cc::vector<flat_expr_id>();
        auto tangents = cc::vector<flat_expr_id>();
        auto any = false;
        for (auto const a : arguments)
        {
            auto const d = derive(a);
            primals.push_back(d.primal);
            tangents.push_back(d.tangent);
            any = any || is_valid(d.tangent);
        }
        auto const primal = add(x.type, flat_construct{.arguments = f.add_list(primals)});
        if (!any)
            return {primal, flat_expr_id::none};
        for (auto i = isize(0); i < tangents.size(); ++i)
            if (!is_valid(tangents[i]))
                tangents[i] = zero(tangent(type_of(primals[i])));
        return {primal, add(tangent(x.type), flat_construct{.arguments = f.add_list(tangents)})};
    }
    if (auto const* const n = x.node.try_as<flat_call>())
        return derive_call(x.type, *n);
    failed("a value it cannot follow yet");
    return {id, flat_expr_id::none};
}

auto differentiator::rule_name(symbol_id callee) const -> cc::string_view
{
    auto const& s = m().at(callee);
    return s.operator_spelling.empty() ? cc::string_view(s.name) : cc::string_view(s.operator_spelling);
}

auto differentiator::is_linear_function(cc::string_view name, isize arity) -> bool
{
    if ((name == "+" || name == "-") && arity <= 2)
        return true;
    cc::string_view const linear[]
        = {"transpose", "matrix_column", "matrix_row", "select", "float", "half", "float2", "float3", "float4",
           "half2",     "half3",         "half4",      "vec3",   "pos3",  "mat2", "mat3",   "mat4"};
    for (auto const l : linear)
        if (name == l)
            return true;
    return false;
}

auto differentiator::componentwise_factors(cc::string_view name,
                                           cc::span<flat_expr_id const> a,
                                           flat_expr_id r,
                                           cc::vector<flat_expr_id>& factors) -> bool
{
    auto const x = [&] { return use(a[0]); };
    auto const t = type_of(r);
    auto const s = scalar_of(t);
    auto const number = [&](f64 v) { return add(s, flat_literal{.value = v}); };
    factors.resize_to_filled(a.size(), flat_expr_id::none);
    if (name == "sin")
        factors[0] = call("cos", x());
    else if (name == "cos")
        factors[0] = negate(call("sin", x()));
    else if (name == "tan")
        factors[0] = call("+", one(t), call("*", use(r), use(r)));
    else if (name == "exp")
        factors[0] = use(r);
    else if (name == "exp2")
        factors[0] = mul(use(r), number(0.69314718055994530942));
    else if (name == "log")
        factors[0] = call("/", one(t), x());
    else if (name == "log2")
        factors[0] = call("/", one(t), mul(x(), number(0.69314718055994530942)));
    else if (name == "sqrt")
        factors[0] = call("/", constant(t, 0.5f), use(r));
    else if (name == "inverse_sqrt")
        factors[0] = mul(call("*", call("*", use(r), use(r)), use(r)), number(-0.5));
    else if (name == "abs")
        factors[0] = call("sign", x());
    else if (name == "fract")
        factors[0] = one(t);
    else if (name == "sinh")
        factors[0] = call("cosh", x());
    else if (name == "cosh")
        factors[0] = call("sinh", x());
    else if (name == "tanh")
        factors[0] = one_minus(call("*", use(r), use(r)));
    else if (name == "asin")
        factors[0] = call("/", one(t), call("sqrt", one_minus(call("*", x(), x()))));
    else if (name == "acos")
        factors[0] = negate(call("/", one(t), call("sqrt", one_minus(call("*", x(), x())))));
    else if (name == "atan")
        factors[0] = call("/", one(t), call("+", one(t), call("*", x(), x())));
    else if (name == "saturate")
        factors[0] = call("*", call("step", zero(t), x()), call("step", x(), one(t)));
    else if (name == "floor" || name == "ceil" || name == "round" || name == "trunc" || name == "sign" || name == "step")
        ;
    else if (name == "pow")
    {
        factors[0] = call("*", use(a[1]), call("pow", x(), call("-", use(a[1]), one(t))));
        factors[1] = call("*", use(r), call("log", x()));
    }
    else if (name == "atan2")
    {
        // atan2(y, x)
        auto const denominator = share(call("+", call("*", use(a[1]), use(a[1])), call("*", use(a[0]), use(a[0]))));
        factors[0] = call("/", use(a[1]), use(denominator));
        factors[1] = negate(call("/", use(a[0]), use(denominator)));
    }
    else if (name == "min" || name == "max")
    {
        // the side that is taken takes the tangent; at a tie the first does
        auto const first = share(name == "min" ? call("step", use(a[0]), use(a[1])) : call("step", use(a[1]), use(a[0])));
        factors[0] = use(first);
        factors[1] = one_minus(use(first));
    }
    else if (name == "clamp")
    {
        auto const above_low = share(call("step", use(a[1]), use(a[0])));
        auto const below_high = share(call("step", use(a[0]), use(a[2])));
        factors[0] = call("*", use(above_low), use(below_high));
        factors[1] = one_minus(use(above_low));
        factors[2] = one_minus(use(below_high));
    }
    else if (name == "mix")
    {
        factors[0] = one_minus(use(a[2]));
        factors[1] = use(a[2]);
        factors[2] = call("-", use(a[1]), use(a[0]));
    }
    else
        return false;
    return true;
}

auto differentiator::transform_position_tangent(cc::span<flat_expr_id const> primals,
                                                cc::span<flat_expr_id const> tangents,
                                                type_id type) -> flat_expr_id
{
    auto const float4 = tangent(type);
    auto const scalar = scalar_of(float4);
    auto const extended = [&](flat_expr_id v, f64 w)
    {
        auto parts = cc::vector<flat_expr_id>();
        for (auto i = 0; i < 3; ++i)
            parts.push_back(add(f.field_type(type_of(v), i), flat_member{.object = use(v), .member = i}));
        parts.push_back(add(scalar, flat_literal{.value = w}));
        return add(float4, flat_construct{.arguments = f.add_list(parts)});
    };
    auto result = flat_expr_id::none;
    if (is_valid(tangents[0]))
        result = call("*", share_tangent(tangents[0]), extended(primals[1], 1.0));
    if (is_valid(tangents[1]))
        result = sum(result, call("*", use(primals[0]), extended(share_tangent(tangents[1]), 0.0)));
    return result;
}

auto differentiator::derive_call(type_id type, flat_call const& n) -> dual
{
    auto const arguments = copy_of(n.arguments);
    auto primals = cc::vector<flat_expr_id>();
    auto tangents = cc::vector<flat_expr_id>();
    auto any = false;
    for (auto const a : arguments)
    {
        auto const d = derive(a);
        primals.push_back(share(d.primal, "a"));
        tangents.push_back(d.tangent);
        any = any || is_valid(d.tangent);
    }
    auto const write_primal = [&]
    {
        auto copies = cc::vector<flat_expr_id>();
        for (auto const p : primals)
            copies.push_back(use(p));
        return f.builtin_call(at, n.callee, copies);
    };
    if (!any)
        return {write_primal(), flat_expr_id::none};
    auto const name = rule_name(n.callee);
    auto const result = share(write_primal(), "r");
    // `m * p` takes the translation, and its tangent does not: a position's change moves through `m` as a direction
    if (m().at(n.callee).name == "transform_position")
        return {use(result), transform_position_tangent(primals, tangents, type)};
    if (m().at(n.callee).name == "transform_position_affine")
    {
        failed("an affine3 times a position, which has no rule yet");
        return {use(result), flat_expr_id::none};
    }

    if (is_linear_function(name, arguments.size()))
    {
        // `x + dx` alone where the other side is zero and of the result's type
        if (name == "+" && arguments.size() == 2 && (!is_valid(tangents[0]) || !is_valid(tangents[1])))
        {
            auto const only = is_valid(tangents[0]) ? tangents[0] : tangents[1];
            if (type_of(only) == type)
                return {use(result), only};
        }
        if (name == "-" && arguments.size() == 2 && !is_valid(tangents[1]) && type_of(tangents[0]) == type)
            return {use(result), tangents[0]};
        auto written = cc::vector<flat_expr_id>();
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (!is_differentiable(type_of(primals[i])))
                written.push_back(use(primals[i]));
            else
                written.push_back(is_valid(tangents[i]) ? tangents[i] : zero(tangent(type_of(primals[i]))));
        }
        // a construction of a position builds its tangent as the direction it is: `pos3(x)`'s is `vec3(dx)`
        if (tangent(type) != type && name == m().name_of(type))
            return {use(result), call(m().name_of(tangent(type)), written)};
        // a tangent of other types than the primal's, as `pos3 + vec3`'s two directions are, takes the builtin of its types
        auto const parameters = m().at(m().functions[m().at(n.callee).info].parameters);
        for (auto i = isize(0); i < written.size(); ++i)
            if (type_of(written[i]) != parameters[i].type)
                return {use(result), call(name, written)};
        return {use(result), f.builtin_call(at, n.callee, written)};
    }

    for (auto& t : tangents)
        t = share_tangent(t);
    auto const tangent_of_argument = [&](isize i) { return use(tangents[i]); };
    auto tangent = flat_expr_id::none;

    if (name == "*")
        tangent = sum(is_valid(tangents[0]) ? mul(tangent_of_argument(0), use(primals[1])) : flat_expr_id::none,
                      is_valid(tangents[1]) ? mul(use(primals[0]), tangent_of_argument(1)) : flat_expr_id::none);
    else if (name == "/")
    {
        // (da - r * db) / b
        auto const r_db = is_valid(tangents[1]) ? mul(use(result), tangent_of_argument(1)) : flat_expr_id::none;
        auto const da = is_valid(tangents[0]) ? tangent_of_argument(0) : flat_expr_id::none;
        tangent = call("/", difference(da, r_db), use(primals[1]));
    }
    else if (name == "dot" || name == "cross")
        tangent = sum(is_valid(tangents[0]) ? call(name, tangent_of_argument(0), use(primals[1])) : flat_expr_id::none,
                      is_valid(tangents[1]) ? call(name, use(primals[0]), tangent_of_argument(1)) : flat_expr_id::none);
    else if (name == "length")
        tangent = call("/", call("dot", use(primals[0]), tangent_of_argument(0)), use(result));
    else if (name == "distance")
    {
        auto const d = share(call("-", use(primals[0]), use(primals[1])));
        auto const dd = difference(is_valid(tangents[0]) ? tangent_of_argument(0) : flat_expr_id::none,
                                   is_valid(tangents[1]) ? tangent_of_argument(1) : flat_expr_id::none);
        tangent = call("/", call("dot", use(d), dd), use(result));
    }
    else if (name == "normalize")
    {
        // (dv - n * dot(n, dv)) / length(v)
        auto const along = mul(use(result), call("dot", use(result), tangent_of_argument(0)));
        tangent = call("/", call("-", tangent_of_argument(0), along), call("length", use(primals[0])));
    }
    else
    {
        auto factors = cc::vector<flat_expr_id>();
        if (!componentwise_factors(name, primals, result, factors))
        {
            failed(cc::format("no derivative of {} yet", name));
            return {use(result), flat_expr_id::none};
        }
        for (auto i = isize(0); i < tangents.size(); ++i)
            if (is_valid(tangents[i]) && is_valid(factors[i]))
                tangent = sum(tangent, mul(tangent_of_argument(i), factors[i]));
    }
    return {use(result), tangent};
}

auto differentiator::root_of(flat_expr_id place, cc::vector<place_step>* steps) -> local_id
{
    auto const x = node(place);
    if (auto const* const r = x.node.try_as<flat_local_ref>())
        return r->local;
    if (auto const* const n = x.node.try_as<flat_member>())
    {
        if (n->is_swizzle())
            return local_id::none;
        auto const root = root_of(n->object, steps);
        if (steps != nullptr)
            steps->push_back({.member = n->member});
        return root;
    }
    if (auto const* const n = x.node.try_as<flat_element>())
    {
        if (!f.is_substitutable(n->index))
            return local_id::none;
        auto const root = root_of(n->object, steps);
        if (steps != nullptr)
            steps->push_back({.index = n->index});
        return root;
    }
    return local_id::none;
}

auto differentiator::is_buffer_place(flat_expr_id place) const -> bool
{
    auto const& x = node(place);
    if (x.node.is<flat_buffer_element>())
        return true;
    if (auto const* const n = x.node.try_as<flat_member>())
        return is_buffer_place(n->object);
    if (auto const* const n = x.node.try_as<flat_element>())
        return is_buffer_place(n->object);
    return false;
}

auto differentiator::element_type(type_id t) const -> type_id
{
    return m().at(t).kind == type_kind::array ? m().at(t).element : f.field_type(t, 0);
}

auto differentiator::place_of(flat_expr_id root, cc::span<place_step const> steps) -> flat_expr_id
{
    auto x = root;
    for (auto const& s : steps)
    {
        if (is_valid(s.index))
            x = add(element_type(type_of(x)), flat_element{.object = x, .index = use(s.index)});
        else
            x = add(f.field_type(type_of(x), s.member), flat_member{.object = x, .member = s.member});
    }
    return x;
}

auto differentiator::forward(cc::span<flat_stmt_id const> statements) -> cc::vector<flat_stmt_id>
{
    auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
    return collect(
        [&]
        {
            for (auto const s : copy)
                forward_stmt(s);
        });
}

auto differentiator::forward_stmt(flat_stmt_id id) -> void
{
    auto const s = f.entry.at(id);
    auto const declare = [&](local_id local, flat_expr_id value, bool is_mut)
    {
        auto const type = f.entry.at(local).type;
        if (!is_differentiable(type))
        {
            f.block.push_back(id);
            return;
        }
        auto const tangent = is_valid(value) ? tangent_of_value(value) : flat_expr_id::none;
        f.block.push_back(id);
        auto const t = f.add_local(is_mut ? local_kind::var : local_kind::let,
                                   cc::format("d_{}", f.entry.at(local).name), this->tangent(type));
        auto const initial = is_valid(tangent) ? tangent : zero(this->tangent(type));
        if (is_mut)
            emit(flat_var{.local = t, .value = initial});
        else
            emit(flat_let{.local = t, .value = initial});
        tangent_of[i32(local)] = t;
    };
    if (auto const* const n = s.node.try_as<flat_let>())
        return declare(n->local, n->value, false);
    if (auto const* const n = s.node.try_as<flat_var>())
        return declare(n->local, n->value, true);
    if (auto const* const n = s.node.try_as<flat_assign>())
    {
        auto steps = cc::vector<place_step>();
        auto const root = root_of(n->place, &steps);
        if (!is_valid(root) && !is_buffer_place(n->place))
            return (void)failed("an assignment to a part of a local it cannot follow yet");
        auto const* const t = is_valid(root) ? tangent_of.get_ptr(i32(root)) : nullptr;
        if (t == nullptr || !is_differentiable(type_of(n->place)))
        {
            f.block.push_back(id);
            return;
        }
        auto const tangent_local = *t;
        // computed before the assignment, which may change what it reads
        auto const tangent = tangent_of_value(n->value);
        auto const value = share(is_valid(tangent) ? tangent : zero(this->tangent(type_of(n->place))), "dt");
        f.block.push_back(id);
        return emit(flat_assign{.place = place_of(ref(tangent_local), steps), .value = use(value)});
    }
    if (auto const* const n = s.node.try_as<flat_if>())
    {
        auto const then_body = f.add_list(forward(n->then_body));
        auto const else_body = f.add_list(forward(n->else_body));
        return restate(id, flat_if{.condition = n->condition, .then_body = then_body, .else_body = else_body});
    }
    if (auto const* const n = s.node.try_as<flat_block>())
    {
        if (auto const* const custom = f.custom_calls.get_ptr(i32(n->label)))
            return forward_custom(id, *custom, n->label);
        return restate(id, flat_block{.label = n->label, .body = f.add_list(forward(n->body))});
    }
    if (auto const* const n = s.node.try_as<flat_loop>())
        return restate(id, flat_loop{.label = n->label, .body = f.add_list(forward(n->body))});
    if (auto const* const n = s.node.try_as<flat_while>())
        return restate(id,
                       flat_while{.label = n->label, .condition = n->condition, .body = f.add_list(forward(n->body))});
    if (auto const* const n = s.node.try_as<flat_for>())
        return restate(id, flat_for{.label = n->label,
                                    .index = n->index,
                                    .first = n->first,
                                    .end = n->end,
                                    .body = f.add_list(forward(n->body))});
    if (auto const* const n = s.node.try_as<flat_case>())
    {
        auto result = *n;
        auto arms = cc::vector<flat_arm>::create_copy_of(cc::span<flat_arm const>(f.entry.at(n->arms)));
        for (auto& arm : arms)
            arm.body = f.add_list(forward(arm.body));
        result.arms = f.add_arms(arms);
        result.default_body = f.add_list(forward(n->default_body));
        return restate(id, result);
    }
    f.block.push_back(id);
}

auto differentiator::resolve_paired(cc::string_view text, cc::string_view which, type_id element)
    -> cc::optional<paired_buffer>
{
    // PROTOTYPE: matched by name, as `@forward(f)` is, since an attribute's argument is no expression resolved yet
    auto const dot = text.find('.');
    auto const binding_name = dot < 0 ? cc::string_view() : text.subview({.offset = 0, .size = dot});
    auto const member_name = dot < 0 ? cc::string_view() : text.subview(dot + 1);
    for (auto i = isize(0); i < m().symbols.size(); ++i)
    {
        auto const& s = m().symbols[i];
        if (s.kind != symbol_kind::binding || s.name != binding_name)
            continue;
        auto const members = m().at(m().bindings[s.info].members);
        for (auto k = isize(0); k < members.size(); ++k)
        {
            if (members[k].name != member_name)
                continue;
            auto const& t = m().at(members[k].type);
            auto const& held = t.kind == type_kind::buffer ? m().at(t.element) : t;
            auto const fits = which == "gradient" ? t.kind == type_kind::buffer && t.is_mut
                                                        && held.kind == type_kind::atomic && held.element == element
                                                  : t.kind == type_kind::buffer && t.element == element;
            if (!fits)
            {
                failed(cc::format("@{}({}) names a {}, where it takes a {} of {}", which, text,
                                  m().name_of(members[k].type),
                                  which == "gradient" ? "mut buffer of atomics" : "buffer", m().name_of(element)));
                return {};
            }
            // the derivative reads or writes a binding the function never named, so the entry point must list it
            auto is_listed = false;
            for (auto const b : f.entry.bindings)
                is_listed = is_listed || b == symbol_id(i);
            if (!is_listed)
            {
                failed(cc::format("@{}({}) names a buffer of {}, which the entry point does not list; list {{{}}} "
                                  "beside the bindings it reads",
                                  which, text, binding_name, binding_name));
                return {};
            }
            return paired_buffer{.binding = symbol_id(i), .member = i32(k), .type = members[k].type};
        }
    }
    failed(cc::format("@{}({}) names no buffer of a binding", which, text));
    return {};
}

auto differentiator::derive_buffer_read(flat_expr_id id, type_id type, flat_buffer_element const& n) -> dual
{
    auto const* const member = node(n.buffer).node.try_as<flat_binding_member>();
    auto const* const info
        = member != nullptr ? &m().at(m().bindings[m().at(member->binding).info].members)[member->member] : nullptr;
    if (info == nullptr || (info->gradient.empty() && info->tangent.empty()))
    {
        failed("a read of a buffer, whose gradient it cannot write back; read it as `no_diff(…)` to hold it "
               "constant, or give the buffer a @gradient");
        return {id, flat_expr_id::none};
    }
    if (m().name_of(type) != builtins::k_float)
    {
        failed(cc::format("a read of a buffer of {}, where a gradient is added by an atomic of a float alone",
                          m().name_of(type)));
        return {id, flat_expr_id::none};
    }
    auto const index = share(n.index, "index");
    auto const primal = add(type, flat_buffer_element{.buffer = n.buffer, .index = use(index)});
    if (!is_reverse)
    {
        // without a @tangent, the buffer holds still in every direction a forward derivative follows
        if (info->tangent.empty())
            return {primal, flat_expr_id::none};
        auto const tangents = resolve_paired(info->tangent, "tangent", type);
        if (!tangents.has_value())
            return {primal, flat_expr_id::none};
        auto const& t = tangents.value();
        auto const buffer = add(t.type, flat_binding_member{.binding = t.binding, .member = t.member});
        return {primal, add(tangent(type), flat_buffer_element{.buffer = buffer, .index = use(index)})};
    }
    if (info->gradient.empty())
    {
        failed("a read of a buffer with a @tangent and no @gradient, where a backward derivative adds the read's "
               "adjoint");
        return {primal, flat_expr_id::none};
    }
    auto const gradients = resolve_paired(info->gradient, "gradient", type);
    if (!gradients.has_value())
        return {primal, flat_expr_id::none};
    auto const placeholder = f.add_local(local_kind::temporary, "t_read", tangent(type));
    linear_locals.insert(i32(placeholder));
    gradient_reads.push_back({.placeholder = placeholder, .gradients = gradients.value(), .index = index});
    return {primal, ref(placeholder)};
}

} // namespace sgl::check::impl
