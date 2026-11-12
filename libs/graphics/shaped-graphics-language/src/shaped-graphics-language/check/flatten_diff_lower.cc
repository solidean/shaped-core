#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/differentiator.hh>

// PROTOTYPE: the differentiator's nodes, calls and lowering: what both modes stand on.

namespace sgl::check::impl
{
auto differentiator::failed(cc::string_view why) -> flat_expr_id
{
    if (!is_failed)
        f.failure_detail = cc::format("{}: {}", is_reverse ? "bwd_diff" : "fwd_diff", why);
    is_failed = true;
    f.mark_failed();
    return flat_expr_id::none;
}

auto differentiator::leaf_count(type_id t) const -> i32
{
    auto const* const r = record_of(t);
    return r != nullptr ? r->leaf_count : -1;
}

auto differentiator::named_type(cc::string_view name) const -> type_id
{
    auto const* const found = f.c.prelude_names.get_ptr(cc::string(name));
    if (found == nullptr)
        return type_id::none;
    for (auto const s : *found)
        if (m().at(s).kind == symbol_kind::structure)
            return m().at(s).type;
    return type_id::none;
}

auto differentiator::scalar_of(type_id t) const -> type_id
{
    if (is_scalar(t))
        return t;
    if (is_vector(t))
        return f.field_type(t, 0);
    return named_type(builtins::k_float);
}

auto differentiator::leaves_of(type_id t, f32 v, cc::vector<scalar>& out) const -> bool
{
    if (auto const* const r = record_of(t))
    {
        for (auto i = 0; i < r->leaf_count; ++i)
        {
            switch (r->leaf_kind)
            {
            case value_kind::scalar_float:
                out.push_back(scalar::of(v));
                break;
            case value_kind::scalar_half:
                out.push_back(scalar::of_half(f64(v)));
                break;
            case value_kind::scalar_uint:
                out.push_back(scalar::of_uint(0));
                break;
            case value_kind::boolean:
                out.push_back(scalar::of(false));
                break;
            case value_kind::none:
                return false;
            default:
                out.push_back({.kind = r->leaf_kind, .bits = 0});
                break;
            }
        }
        return true;
    }
    auto const& info = m().at(t);
    if (info.kind == type_kind::enumeration)
    {
        auto const cases = m().at(info.cases);
        if (cases.empty())
            return false;
        out.push_back(scalar::of(cases[0].value));
        return true;
    }
    if (info.kind == type_kind::array)
    {
        for (auto i = 0; i < info.count; ++i)
            if (!leaves_of(info.element, v, out))
                return false;
        return info.count > 0;
    }
    if (info.kind != type_kind::structure)
        return false;
    for (auto const& member : m().at(info.members))
        if (!leaves_of(member.type, v, out))
            return false;
    return true;
}

auto differentiator::constant(type_id t, f32 v) -> flat_expr_id
{
    auto leaves = cc::vector<scalar>();
    if (!leaves_of(t, v, leaves))
        return failed(cc::format("a value of {} has no constant", m().name_of(t)));
    auto at_leaf = isize(0);
    return f.value_of(t, at, leaves, at_leaf);
}

auto differentiator::copy_of(ast::range_of<flat_stmt_id> r) const -> cc::vector<flat_stmt_id>
{
    return cc::vector<flat_stmt_id>::create_copy_of(cc::span<flat_stmt_id const>(f.entry.at(r)));
}

auto differentiator::copy_of(ast::range_of<flat_expr_id> r) const -> cc::vector<flat_expr_id>
{
    return cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(f.entry.at(r)));
}

auto differentiator::share(flat_expr_id x, cc::string_view name) -> flat_expr_id
{
    if (!is_valid(x) || f.is_substitutable(x))
        return x;
    if (is_reverse && is_linear(x))
        return share_tangent(x);
    auto const local = f.add_local(local_kind::temporary, name, type_of(x));
    emit(flat_let{.local = local, .value = x});
    return ref(local);
}

auto differentiator::share_tangent(flat_expr_id t) -> flat_expr_id
{
    if (!is_valid(t) || !is_reverse || f.is_substitutable(t))
        return share(t, "dt");
    auto const local = f.add_local(local_kind::temporary, "linear", type_of(t));
    linear_definition[i32(local)] = t;
    linear_lets.push_back(local);
    linear_locals.insert(i32(local));
    return ref(local);
}

auto differentiator::clone(flat_expr_id id) -> flat_expr_id
{
    if (!is_valid(id))
        return id;
    auto x = node(id);
    if (auto const* const r = x.node.try_as<flat_local_ref>())
        if (auto const* const twin = substitution.get_ptr(i32(r->local)))
            return ref(*twin);
    if (x.node.is<flat_block>() || x.node.is<flat_by_target>())
        return failed("a value that still holds a block");
    if (auto* const n = x.node.try_as<flat_member>())
        n->object = clone(n->object);
    else if (auto* const n = x.node.try_as<flat_element>())
    {
        n->object = clone(n->object);
        n->index = clone(n->index);
    }
    else if (auto* const n = x.node.try_as<flat_buffer_element>())
    {
        n->buffer = clone(n->buffer);
        n->index = clone(n->index);
    }
    else if (auto* const n = x.node.try_as<flat_buffer_length>())
        n->buffer = clone(n->buffer);
    else if (auto* const n = x.node.try_as<flat_construct>())
    {
        auto arguments = copy_of(n->arguments);
        for (auto& a : arguments)
            a = clone(a);
        n->arguments = f.add_list(arguments);
    }
    else if (auto* const n = x.node.try_as<flat_call>())
    {
        auto arguments = copy_of(n->arguments);
        for (auto& a : arguments)
            a = clone(a);
        n->arguments = f.add_list(arguments);
    }
    else if (auto* const n = x.node.try_as<flat_not>())
        n->operand = clone(n->operand);
    else if (auto* const n = x.node.try_as<flat_and>())
    {
        n->lhs = clone(n->lhs);
        n->rhs = clone(n->rhs);
    }
    else if (auto* const n = x.node.try_as<flat_or>())
    {
        n->lhs = clone(n->lhs);
        n->rhs = clone(n->rhs);
    }
    f.entry.exprs.push_back(cc::move(x));
    return flat_expr_id(f.entry.exprs.size() - 1);
}

auto differentiator::is_operator(cc::string_view name) -> bool
{
    return !name.empty() && !(name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'));
}

auto differentiator::find(cc::string_view name, cc::span<type_id const> types) const -> symbol_id
{
    if (is_operator(name))
        return f.c.find_operator(f.file(), name, types);
    auto const* const found = f.c.prelude_names.get_ptr(cc::string(name));
    if (found == nullptr)
        return symbol_id::none;
    for (auto const candidate : *found)
    {
        auto const& s = m().at(candidate);
        if (s.kind != symbol_kind::function || s.info < 0 || !is_valid(s.intrinsic))
            continue;
        auto const parameters = m().at(m().functions[s.info].parameters);
        auto is_match = parameters.size() == types.size();
        for (auto i = isize(0); is_match && i < parameters.size(); ++i)
            is_match = parameters[i].type == types[i];
        if (is_match)
            return candidate;
    }
    return symbol_id::none;
}

auto differentiator::call(cc::string_view name, cc::span<flat_expr_id const> arguments) -> flat_expr_id
{
    auto types = cc::vector<type_id>();
    for (auto const a : arguments)
    {
        if (!is_valid(a))
            return failed(cc::format("a derivative of {} met a value it could not write", name));
        types.push_back(type_of(a));
    }
    auto const callee = find(name, types);
    if (!is_valid(callee))
    {
        auto spelled = cc::string();
        for (auto const t : types)
            spelled += cc::format("{}{}", spelled.empty() ? "" : ", ", m().name_of(t));
        return failed(cc::format("its derivative needs {}({}), which no builtin is", name, spelled));
    }
    return f.builtin_call(at, callee, arguments);
}

auto differentiator::call(cc::string_view name, flat_expr_id a) -> flat_expr_id
{
    flat_expr_id const arguments[] = {a};
    return call(name, arguments);
}

auto differentiator::call(cc::string_view name, flat_expr_id a, flat_expr_id b) -> flat_expr_id
{
    flat_expr_id const arguments[] = {a, b};
    return call(name, arguments);
}

auto differentiator::call(cc::string_view name, flat_expr_id a, flat_expr_id b, flat_expr_id c) -> flat_expr_id
{
    flat_expr_id const arguments[] = {a, b, c};
    return call(name, arguments);
}

auto differentiator::mul(flat_expr_id a, flat_expr_id b) -> flat_expr_id
{
    if (!is_valid(a) || !is_valid(b))
        return flat_expr_id::none;
    type_id const ab[] = {type_of(a), type_of(b)};
    type_id const ba[] = {type_of(b), type_of(a)};
    if (!is_valid(find("*", ab)) && (is_scalar(ab[0]) || is_scalar(ab[1])) && is_valid(find("*", ba)))
        return call("*", b, a);
    return call("*", a, b);
}

auto differentiator::sum(flat_expr_id a, flat_expr_id b) -> flat_expr_id
{
    if (!is_valid(a))
        return b;
    if (!is_valid(b))
        return a;
    return call("+", a, b);
}

auto differentiator::difference(flat_expr_id a, flat_expr_id b) -> flat_expr_id
{
    if (!is_valid(b))
        return a;
    if (!is_valid(a))
        return negate(b);
    return call("-", a, b);
}

auto differentiator::lower_list(cc::span<flat_stmt_id const> statements) -> void
{
    for (auto const s : cc::vector<flat_stmt_id>::create_copy_of(statements))
        lower_stmt(s);
}

auto differentiator::lowered(ast::range_of<flat_stmt_id> body) -> cc::vector<flat_stmt_id>
{
    auto const statements = copy_of(body);
    return collect([&] { lower_list(statements); });
}

auto differentiator::lower_stmt(flat_stmt_id id) -> void
{
    auto const s = f.entry.at(id);
    if (auto const* const n = s.node.try_as<flat_let>())
        return restate(id, flat_let{.local = n->local, .value = lift(n->value)});
    if (auto const* const n = s.node.try_as<flat_var>())
        return restate(id, flat_var{.local = n->local, .value = lift(n->value)});
    if (auto const* const n = s.node.try_as<flat_assign>())
        return restate(id, flat_assign{.place = n->place, .value = lift(n->value)});
    if (auto const* const n = s.node.try_as<flat_eval>())
    {
        auto const value = lift(n->value);
        auto const* const r = is_valid(value) ? node(value).node.try_as<flat_local_ref>() : nullptr;
        if (is_valid(value) && (r == nullptr || !is_lifted_result(r->local)))
            restate(id, flat_eval{.value = value});
        return;
    }
    if (auto const* const n = s.node.try_as<flat_leave>())
    {
        if (auto const* const r = lifted.get_ptr(i32(n->target)); r != nullptr && is_valid(n->value))
        {
            auto const result = *r;
            auto const value = lift(n->value);
            restate(id, flat_assign{.place = ref(result), .value = value});
            return restate(id, flat_leave{.target = n->target});
        }
        return restate(id, flat_leave{.target = n->target, .value = lift(n->value)});
    }
    if (auto const* const n = s.node.try_as<flat_if>())
    {
        auto const condition = lift(n->condition);
        auto const then_body = f.add_list(lowered(n->then_body));
        auto const else_body = f.add_list(lowered(n->else_body));
        return restate(id, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
    }
    if (auto const* const n = s.node.try_as<flat_block>())
        return restate(id, flat_block{.label = n->label, .body = f.add_list(lowered(n->body))});
    if (auto const* const n = s.node.try_as<flat_loop>())
        return restate(id, flat_loop{.label = n->label, .body = f.add_list(lowered(n->body))});
    if (auto const* const n = s.node.try_as<flat_while>())
        return restate(id,
                       flat_while{.label = n->label, .condition = n->condition, .body = f.add_list(lowered(n->body))});
    if (auto const* const n = s.node.try_as<flat_for>())
        return restate(id, flat_for{.label = n->label,
                                    .index = n->index,
                                    .first = n->first,
                                    .end = n->end,
                                    .body = f.add_list(lowered(n->body))});
    if (auto const* const n = s.node.try_as<flat_case>())
    {
        auto result = *n;
        result.scrutinee = lift(n->scrutinee);
        auto arms = cc::vector<flat_arm>::create_copy_of(cc::span<flat_arm const>(f.entry.at(n->arms)));
        for (auto& arm : arms)
            arm.body = f.add_list(lowered(arm.body));
        result.arms = f.add_arms(arms);
        result.default_body = f.add_list(lowered(n->default_body));
        return restate(id, result);
    }
    f.block.push_back(id);
}

auto differentiator::is_lifted_result(local_id local) const -> bool
{
    for (auto const& [label, result] : lifted)
        if (result == local)
            return true;
    return false;
}

auto differentiator::lift(flat_expr_id id) -> flat_expr_id
{
    if (!is_valid(id))
        return id;
    auto x = node(id);
    if (auto const* const b = x.node.try_as<flat_block>())
    {
        auto const block_where = origin{.file = x.from.file, .expr = x.from.expr};
        auto const label = b->label;
        auto const body = b->body;
        if (x.type == checked_module::void_type)
        {
            auto const statements = lowered(body);
            f.add_stmt(block_where, flat_block{.label = label, .body = f.add_list(statements)});
            return flat_expr_id::none;
        }
        auto const result = f.add_local(local_kind::var, f.entry.at(label).name, x.type);
        auto leaves = cc::vector<scalar>();
        auto const initial = leaves_of(x.type, 0.0f, leaves) ? zero(x.type) : flat_expr_id::none;
        f.add_stmt(block_where, flat_var{.local = result, .value = initial});
        lifted[i32(label)] = result;
        auto const statements = lowered(body);
        f.add_stmt(block_where, flat_block{.label = label, .body = f.add_list(statements)});
        return ref(result);
    }
    if (x.node.is<flat_by_target>())
        return failed("a trace, whose two forms it cannot follow yet");
    if (auto* const n = x.node.try_as<flat_member>())
        n->object = lift(n->object);
    else if (auto* const n = x.node.try_as<flat_element>())
    {
        n->object = lift(n->object);
        n->index = lift(n->index);
    }
    else if (auto* const n = x.node.try_as<flat_buffer_element>())
        n->index = lift(n->index);
    else if (auto* const n = x.node.try_as<flat_construct>())
    {
        auto arguments = copy_of(n->arguments);
        for (auto& a : arguments)
            a = lift(a);
        n->arguments = f.add_list(arguments);
    }
    else if (auto* const n = x.node.try_as<flat_call>())
    {
        auto arguments = copy_of(n->arguments);
        for (auto& a : arguments)
            a = lift(a);
        n->arguments = f.add_list(arguments);
    }
    else if (auto* const n = x.node.try_as<flat_not>())
        n->operand = lift(n->operand);
    else if (auto* const n = x.node.try_as<flat_and>())
        n->lhs = lift(n->lhs);
    else if (auto* const n = x.node.try_as<flat_or>())
        n->lhs = lift(n->lhs);
    f.entry.exprs[index_of(id)] = cc::move(x);
    return id;
}

} // namespace sgl::check::impl
