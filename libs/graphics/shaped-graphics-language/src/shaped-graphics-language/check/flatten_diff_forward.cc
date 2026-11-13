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
        // a swizzle's letters name the fields, so only a struct that is its own tangent keeps their meaning
        auto const object_type = type_of(object.primal);
        if (n->is_swizzle() && tangent(object_type) != object_type && record_of(object_type) == nullptr)
            return {primal, failed("a swizzle of a struct whose tangent is another struct")};
        auto const member = n->is_swizzle() ? n->member : tangent_member(object_type, n->member);
        if (!n->is_swizzle() && member < 0)
            return {primal, flat_expr_id::none};
        return {primal,
                add(tangent(x.type), flat_member{.object = object.tangent, .member = member, .letters = n->letters})};
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
        // a struct's tangent holds its differentiable fields alone
        auto kept = cc::vector<flat_expr_id>();
        for (auto i = isize(0); i < tangents.size(); ++i)
        {
            if (!is_part_kept(x.type, i))
                continue;
            kept.push_back(is_valid(tangents[i]) ? tangents[i] : zero(tangent(type_of(primals[i]))));
        }
        return {primal, add(tangent(x.type), flat_construct{.arguments = f.add_list(kept)})};
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
        = {"transpose", "matrix_column", "matrix_row", "select",          "float",      "half", "float2",
           "float3",    "float4",        "half2",      "half3",           "half4",      "vec3", "pos3",
           "mat2",      "mat3",          "mat4",       "affine3_of_rows", "affine3_row"};
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

auto differentiator::affine_tangent(cc::span<flat_expr_id const> primals,
                                    cc::span<flat_expr_id const> tangents,
                                    type_id type,
                                    f64 w) -> flat_expr_id
{
    auto const float4 = named_type("float4");
    auto const scalar = named_type(builtins::k_float);
    // a position is (p, 1) to the matrix and its change (dp, 0); a direction is (v, 0) and a float4 itself
    auto const extended = [&](flat_expr_id v, f64 last)
    {
        if (type_of(v) == float4)
            return use(v);
        auto parts = cc::vector<flat_expr_id>();
        for (auto i = 0; i < 3; ++i)
            parts.push_back(add(f.field_type(type_of(v), i), flat_member{.object = use(v), .member = i}));
        parts.push_back(add(scalar, flat_literal{.value = last}));
        return add(float4, flat_construct{.arguments = f.add_list(parts)});
    };
    auto const dm = is_valid(tangents[0]) ? share_tangent(tangents[0]) : flat_expr_id::none;
    auto const dv = is_valid(tangents[1]) ? share_tangent(tangents[1]) : flat_expr_id::none;
    auto parts = cc::vector<flat_expr_id>();
    for (auto i = 0; i < 3; ++i)
    {
        auto part = flat_expr_id::none;
        if (is_valid(dm))
            part = call("dot", call("affine3_row", use(dm), integer(i)), extended(primals[1], w));
        if (is_valid(dv))
            part = sum(part, call("dot", call("affine3_row", use(primals[0]), integer(i)), extended(dv, 0.0)));
        parts.push_back(part);
    }
    return add(tangent(type), flat_construct{.arguments = f.add_list(parts)});
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
        // a resource stands as itself, since no target holds one in a local
        primals.push_back(is_resource(m().at(type_of(d.primal)).kind) ? d.primal : share(d.primal, "a"));
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
    // a texture's texels change too, which no argument says
    if (auto const* const record = m().builtin_function(n.intrinsic);
        record != nullptr && record->is_texture_call && !primals.empty()
        && m().at(type_of(primals[0])).kind == type_kind::texture)
    {
        auto const result = share(write_primal(), "r");
        return {use(result), texture_tangent(n, primals, tangents, type)};
    }
    if (!any)
        return {write_primal(), flat_expr_id::none};
    auto const name = rule_name(n.callee);
    auto const result = share(write_primal(), "r");
    // `m * p` takes the translation, and its tangent does not: a position's change moves through `m` as a direction
    if (m().at(n.callee).name == "transform_position")
        return {use(result), transform_position_tangent(primals, tangents, type)};
    // an affine3's products are dots of its rows, which reverse mode transposes as it does any dot
    if (auto const& callee = m().at(n.callee).name; callee == "transform_position_affine"
                                                    || callee == "transform_direction_affine"
                                                    || callee == "transform_float4_affine")
        return {use(result), affine_tangent(primals, tangents, type, callee == "transform_position_affine" ? 1.0 : 0.0)};

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

auto differentiator::tangent_place_of(flat_expr_id root, cc::span<place_step const> steps) -> flat_expr_id
{
    auto x = root;
    for (auto const& s : steps)
    {
        if (is_valid(s.index))
        {
            x = add(element_type(type_of(x)), flat_element{.object = x, .index = use(s.index)});
            continue;
        }
        auto const primal = m().at(type_of(x)).tangent_of;
        auto const member = is_valid(primal) ? tangent_member(primal, s.member) : s.member;
        if (member < 0)
            return flat_expr_id::none;
        x = add(f.field_type(type_of(x), member), flat_member{.object = x, .member = member});
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
        auto const place = tangent_place_of(ref(tangent_local), steps);
        if (is_valid(place))
            emit(flat_assign{.place = place, .value = use(value)});
        return;
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

auto differentiator::texture_builtin(cc::string_view name, type_id texture, isize arity) const -> symbol_id
{
    auto const* const found = f.c.prelude_names.get_ptr(cc::string(name));
    if (found == nullptr)
        return symbol_id::none;
    auto by_shape = symbol_id::none;
    for (auto const candidate : *found)
    {
        auto const& s = m().at(candidate);
        if (s.kind != symbol_kind::function || s.info < 0 || !is_valid(s.intrinsic))
            continue;
        auto const parameters = m().at(m().functions[s.info].parameters);
        if (parameters.size() != arity)
            continue;
        if (parameters[0].type == texture)
            return candidate;
        // CHK-194: a bare `texture_2d` takes every texture of its shape
        auto const& p = m().at(parameters[0].type);
        if (p.kind == type_kind::texture && p.shape == m().at(texture).shape && !is_valid(p.element) && !p.is_depth)
            by_shape = candidate;
    }
    return by_shape;
}

auto differentiator::prelude_call(cc::string_view name, cc::span<flat_expr_id const> arguments) -> flat_expr_id
{
    auto const* const found = f.c.prelude_names.get_ptr(cc::string(name));
    if (found == nullptr || found->empty())
        return failed(cc::format("the prelude's {}, which it cannot find", name));
    auto const callee = found->front();
    auto const inlined = f.inline_call(at, callee, arguments);
    if (!is_valid(inlined.label))
        return failed(cc::format("the prelude's {}, which did not inline", name));
    return add(m().functions[m().at(callee).info].result, flat_block{.label = inlined.label, .body = inlined.body});
}

auto differentiator::texture_tangent(flat_call const& n,
                                     cc::span<flat_expr_id const> primals,
                                     cc::span<flat_expr_id const> tangents,
                                     type_id type) -> flat_expr_id
{
    auto const& callee = m().at(n.callee);
    auto const parameters = m().at(m().functions[callee.info].parameters);
    auto const is_load = callee.name == "load" && primals.size() == 3;
    auto const is_sample = callee.name == "sample" && primals.size() == 4 && parameters[2].name == "smp"
                        && parameters[3].name == "level";
    auto const texture_type = type_of(primals[0]);
    auto const* const member = node(primals[0]).node.try_as<flat_binding_member>();
    auto const* const info
        = member != nullptr ? &m().at(m().bindings[m().at(member->binding).info].members)[member->member] : nullptr;
    if ((!is_load && !is_sample) || info == nullptr || m().at(texture_type).shape != texture_shape::d2
        || float_leaves(type) <= 0)
        return failed("a texture read other than a texture_2d member's `load` or its `sample` at a level, which it "
                      "cannot differentiate yet");
    if (info->is_no_diff && info->gradient.empty() && info->tangent.empty() && !is_valid(tangents[1]))
        return flat_expr_id::none;
    if (!info->is_no_diff && info->gradient.empty() && info->tangent.empty())
        return failed("a read of a texture, whose gradient it cannot write back; give the texture a @gradient, or "
                      "mark it @no_diff to hold its texels constant");

    // the sampler's filter and address, which a static sampler alone says at compile time
    auto is_linear = false;
    auto mode_u = 0;
    auto mode_v = 0;
    if (is_sample)
    {
        auto state = static_cast<sampler_state const*>(nullptr);
        auto const& smp = node(primals[2]).node;
        if (auto const* const m_ = smp.try_as<flat_binding_member>())
        {
            auto const& s = m().at(m().bindings[m().at(m_->binding).info].members)[m_->member];
            if (s.static_sampler >= 0)
                state = &m().samplers[s.static_sampler];
        }
        else if (auto const* const file = smp.try_as<flat_file_sampler>())
            state = &m().samplers[m().at(file->sampler).info];
        if (state == nullptr)
            return failed("a sample through a sampler whose filter the compile cannot see; a static sampler says it");
        auto const* const level = node(primals[3]).node.try_as<flat_literal>();
        if (level == nullptr || level->value != 0.0 || state->max_anisotropy > 1 || state->mip_lod_bias != 0.0f
            || state->min_lod > 0.0f)
            return failed("a sample at a level other than a constant 0, or through a sampler that moves it");
        is_linear = state->mag_filter == 1;
        mode_u = state->address_u;
        mode_v = state->address_v;
    }
    else if (auto const level = f.constant_int(primals[2]); is_reverse && (!level.has_value() || level.value() != 0))
        return failed("a load at a level other than a constant 0, whose gradient has no texels to go to yet");

    auto const load = texture_builtin("load", texture_type, 3);
    auto const size_of = texture_builtin("size", texture_type, 2);
    if (!is_valid(load) || !is_valid(size_of))
        return failed(cc::format("a load of {}, which no builtin is", m().name_of(texture_type)));
    auto const taps = is_sample && is_linear ? 4 : 1;
    auto const size
        = is_sample
            ? share(f.builtin_call(at, size_of, cc::span<flat_expr_id const>({use(primals[0]), integer(0)})), "size")
            : flat_expr_id::none;
    auto const tap_texel = [&](i32 k)
    {
        if (is_load)
            return use(primals[1]);
        flat_expr_id const arguments[]
            = {use(size), use(primals[1]), integer(k), boolean(is_linear), integer(mode_u), integer(mode_v)};
        return prelude_call("sample_tap_texel", arguments);
    };
    auto const tap_weight = [&](i32 k)
    {
        flat_expr_id const arguments[] = {use(size), use(primals[1]), integer(k), boolean(is_linear)};
        return prelude_call("sample_tap_weight", arguments);
    };
    auto const level = is_load ? use(primals[2]) : integer(0);

    auto result = flat_expr_id::none;
    // the coordinate's change moves each tap's weight, and with it how much of its texel the sample holds
    if (is_sample && is_linear && is_valid(tangents[1]))
    {
        auto const dc = share_tangent(tangents[1]);
        for (auto k = 0; k < taps; ++k)
        {
            flat_expr_id const slope_arguments[] = {use(size), use(primals[1]), integer(k), boolean(is_linear)};
            auto const slope = prelude_call("sample_tap_slope", slope_arguments);
            auto const texel
                = f.builtin_call(at, load, cc::span<flat_expr_id const>({use(primals[0]), tap_texel(k), integer(0)}));
            result = sum(result, mul(texel, call("dot", slope, use(dc))));
        }
    }
    if (!is_reverse)
    {
        if (info->tangent.empty())
            return result;
        auto const tangents_texture = resolve_paired(info->tangent, "tangent", texture_type);
        if (!tangents_texture.has_value())
            return result;
        auto const& t = tangents_texture.value();
        for (auto k = 0; k < taps; ++k)
        {
            auto const changed = add(t.type, flat_binding_member{.binding = t.binding, .member = t.member});
            auto const texel
                = f.builtin_call(at, load, cc::span<flat_expr_id const>({changed, tap_texel(k), use(level)}));
            result = sum(result, taps == 1 ? texel : mul(texel, tap_weight(k)));
        }
        return result;
    }
    if (info->gradient.empty())
    {
        if (info->is_no_diff)
            return result;
        return failed("a read of a texture with a @tangent and no @gradient, where a backward derivative adds the "
                      "read's adjoint");
    }
    auto const gradients = resolve_paired(info->gradient, "gradient", named_type(builtins::k_float));
    if (!gradients.has_value())
        return result;
    auto const placeholder = f.add_local(local_kind::temporary, "t_texels", tangent(type));
    linear_locals.insert(i32(placeholder));
    texture_reads.push_back({.placeholder = placeholder,
                             .gradients = gradients.value(),
                             .texture = primals[0],
                             .at = primals[1],
                             .is_load = is_load,
                             .is_linear = is_linear,
                             .mode_u = mode_u,
                             .mode_v = mode_v});
    return sum(result, ref(placeholder));
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
                            : t.kind == type_kind::buffer ? t.element == element
                                                          : members[k].type == element;
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
    // a gradient is an atomic per float the element's tangent holds, which no other kind of leaf has
    auto const stride = float_leaves(tangent(type));
    if (stride <= 0)
    {
        failed(cc::format("a read of a buffer of {}, whose gradient is an atomic float per float it holds, and it "
                          "holds another kind",
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
        auto const tangents = resolve_paired(info->tangent, "tangent", tangent(type));
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
    auto const gradients = resolve_paired(info->gradient, "gradient", named_type(builtins::k_float));
    if (!gradients.has_value())
        return {primal, flat_expr_id::none};
    auto const placeholder = f.add_local(local_kind::temporary, "t_read", tangent(type));
    linear_locals.insert(i32(placeholder));
    gradient_reads.push_back(
        {.placeholder = placeholder, .gradients = gradients.value(), .index = index, .stride = stride});
    return {primal, ref(placeholder)};
}

} // namespace sgl::check::impl
