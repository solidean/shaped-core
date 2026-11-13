#include <shaped-graphics-language/check/impl/flattener.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

auto sgl::check::impl::flattener::flatten_expr(ast::expr_id id) -> flat_expr_id
{
    if (!ast::is_valid(id))
        return fail();
    auto const at = cursor_scope(*this, origin{.file = file(), .expr = id});
    auto const& e = ast().at(id);
    auto const type = tables().type_at(id);
    auto const& where = tables().target_at(id);
    meets_error = meets_error || holds_error(type);
    if (!is_valid(type))
        return fail();

    if (e.node.is<ast::literal>())
        return flatten_number(id, type);
    // CHK-394: an operator over number literals alone is the literal it folds to
    if (e.node.is<ast::call>())
    {
        if (auto const n = c.number_of(file(), id); n.is_number)
            return flatten_folded(id, type, n);
        if (auto const b = c.comparison_of(file(), id); b.has_value())
            return add_expr(type, id, flat_bool_literal{.value = b.value()});
    }
    // a tuple or an object literal is the call it converts by (CHK-81)
    if ((e.node.is<ast::tuple>() || e.node.is<ast::object>()) && tables().call_at(id) >= 0)
        return flatten_bound_call(id, type, c.out.call_records[tables().call_at(id)]);
    // void's one value is the construction of no fields.
    if (e.node.is<ast::void_ref>())
        return add_expr(type, id, flat_construct{});
    if (e.node.is<ast::leading_dot>())
    {
        return where.kind == target_kind::enum_case ? enum_value(type, id, where.index) : fail();
    }
    // A module-level name: a const is its value, and a file-scope sampler is handed to a builtin.
    // A const that did not check has no value, and its name already has the error type (CHK-19).
    auto const module_level = [&]() -> flat_expr_id
    {
        if (where.kind == target_kind::symbol && c.out.at(where.symbol).kind == symbol_kind::constant
            && c.out.at(where.symbol).state == symbol_state::checked)
        {
            // CHK-426: the target is this compile's, which no const holds
            if (auto const index = target_case_of(where.symbol); index >= 0)
                return refuses_target_in_const(c.out.at(where.symbol).name) ? flat_expr_id::none
                                                                            : enum_value(type, id, index);
            return constant_value(type, id, c.out.constants[c.out.at(where.symbol).info]);
        }
        if (where.kind == target_kind::symbol && c.out.at(where.symbol).kind == symbol_kind::sampler
            && c.out.at(where.symbol).state == symbol_state::checked)
            return add_expr(type, id, flat_file_sampler{.sampler = where.symbol});
        return fail();
    };
    // CHK-338: a value parameter's name in an instance is the constant it stands for
    if (where.kind == target_kind::generic_value)
    {
        auto const& constant = c.out.types[where.index];
        if (c.out.at(constant.element).kind == type_kind::enumeration)
            return enum_value(type, id, constant.count);
        auto const element = c.out.name_of(constant.element);
        auto const kind = element == "float" ? value_kind::scalar_float
                        : element == "uint"  ? value_kind::scalar_uint
                        : element == "bool"  ? value_kind::boolean
                                             : value_kind::scalar_int;
        scalar const leaf[] = {{.kind = kind, .bits = u32(constant.count)}};
        auto at = isize(0);
        return value_of(type, id, leaf, at);
    }
    if (e.node.is<ast::name>() || e.node.is<ast::self_ref>())
    {
        for (auto const& b : current()->bound)
            if (b.where == where)
                return is_valid(b.literal) ? again(b.literal, id) : local_ref(b.local, id);
        return module_level();
    }
    if (auto const* const m = e.node.try_as<ast::member>())
    {
        // CHK-348: `m.name`, a module-level name of module `m`
        if (where.kind == target_kind::symbol)
            return module_level();
        // `a.foo` that is no field is a call of `foo` with `a` (CHK-249)
        if (tables().call_at(id) >= 0)
            return flatten_bound_call(id, type, c.out.call_records[tables().call_at(id)]);
        if (where.kind == target_kind::enum_case)
            return enum_value(type, id, where.index);
        if (where.kind == target_kind::binding_member)
            return add_expr(type, id,
                            flat_binding_member{.binding = where.symbol,
                                                .member = where.index,
                                                .is_workgroup = c.is_workgroup_binding(where.symbol)});
        if (where.kind == target_kind::buffer_length)
            return add_expr(type, id, flat_buffer_length{.buffer = flatten_expr(m->object)});
        // CHK-288: a constant, and an object with an effect still runs for it
        if (where.kind == target_kind::array_length)
        {
            auto const object = flatten_expr(m->object);
            if (calls_impure(object, 0))
                add_stmt({.file = file(), .expr = id}, flat_eval{.value = object});
            return add_expr(type, id, flat_int_literal{.value = where.index});
        }
        if (is_swizzled(id))
            return read_swizzle(swizzled_at(id), type, id);
        if (where.kind != target_kind::field)
            return fail();
        auto const object = flatten_expr(m->object);
        return add_expr(type, id, flat_member{.object = object, .member = where.index});
    }
    if (auto const* const indexed = e.node.try_as<ast::index>())
    {
        auto const arguments = ast().at(indexed->arguments);
        auto const object_type = tables().type_at(indexed->object);
        // CHK-367, CHK-514: a texel read is the `load` the check pass resolved it as
        if (is_valid(object_type)
            && (c.out.at(object_type).kind == type_kind::image || c.out.at(object_type).kind == type_kind::texture))
            return tables().call_at(id) >= 0 && !c.out.at(object_type).is_atomic
                     ? flatten_bound_call(id, type, c.out.call_records[tables().call_at(id)])
                     : fail();
        // CHK-395: a lane of a vector is an element of it, which every target subscripts as it does an array
        if (auto const lane = is_valid(object_type) ? c.lane_type_of(object_type) : type_id::none;
            is_valid(lane) && arguments.size() == 1)
            return add_expr(
                lane, id,
                flat_element{.object = flatten_expr(indexed->object), .index = flatten_expr(arguments[0].value)});
        // CHK-287: one element per index, `grid[i, j]` being `grid[i][j]`
        if (is_valid(object_type) && c.out.at(object_type).kind == type_kind::array)
        {
            auto object = flatten_expr(indexed->object);
            auto element_type = object_type;
            for (auto const& a : arguments)
            {
                element_type = c.out.at(element_type).element;
                auto const index = flatten_expr(a.value);
                object = add_expr(element_type, id, flat_element{.object = object, .index = index});
            }
            return object;
        }
        // The check pass let only a buffer element through otherwise, so the object is a resource.
        if (arguments.size() != 1)
            return fail();
        auto const buffer = flatten_expr(indexed->object);
        auto const index = flatten_expr(arguments[0].value);
        return add_expr(type, id, flat_buffer_element{.buffer = buffer, .index = index});
    }
    if (auto const* const literal = e.node.try_as<ast::array>())
        return flatten_array_literal(id, type, *literal);
    if (auto const* const call = e.node.try_as<ast::call>())
        return flatten_call(id, type, where, *call);
    if (auto const* const cast = e.node.try_as<ast::cast>())
    {
        auto const value = flatten_expr(cast->value);
        if (where.kind == target_kind::texel_conversion)
            return texel_conversion(value, type, id);
        // A cast to the type the value already has is the value itself.
        if (where.kind != target_kind::overload)
            return value;
        flat_expr_id const arguments[] = {value};
        // A conversion the program declares is inlined like any call of it (CHK-195).
        if (!is_valid(c.out.at(where.symbol).intrinsic))
        {
            auto const inlined = inline_call(id, where.symbol, arguments);
            return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
        }
        return builtin_call(id, where.symbol, arguments);
    }
    if (auto const* const chain = e.node.try_as<ast::comparison_chain>())
        return flatten_chain(id, type, *chain);
    if (auto const* const loop = e.node.try_as<ast::loop_expr>())
        return flatten_value_loop(id, type, *loop);
    if (auto const* const c = e.node.try_as<ast::case_expr>())
        return flatten_value_case(id, type, *c);
    if (auto const* const i = e.node.try_as<ast::if_expr>())
        return flatten_value_if(id, type, *i);
    return fail();
}

auto sgl::check::impl::flattener::enum_value(type_id type, ast::expr_id from, i32 case_index) -> flat_expr_id
{
    if (c.out.is_plain_enum(type))
        return add_expr(type, from, flat_enum_value{.case_index = case_index});
    auto const cases = c.out.at(c.out.at(type).cases);
    if (case_index < 0 || case_index >= cases.size())
        return fail();
    auto const value = cases[case_index].value;
    if (c.out.builtin_type_of(type)->leaf_kind == value_kind::boolean)
        return add_expr(type, from, flat_bool_literal{.value = value != 0});
    return add_expr(type, from, flat_int_literal{.value = i32(value)});
}

auto sgl::check::impl::flattener::constant_value(type_id type, ast::expr_id from, constant_info const& info)
    -> flat_expr_id
{
    switch (info.kind)
    {
    case constant_kind::integer:
        return add_expr(type, from,
                        flat_int_literal{.value = info.integer, .is_unsigned = info.is_unsigned, .is_hex = info.is_hex});
    case constant_kind::real:
        return add_expr(type, from, flat_literal{.value = info.real});
    case constant_kind::enum_case:
        return enum_value(type, from, info.case_index);
    case constant_kind::composite:
    {
        // EMIT-165: an array is bound once per tree and read from there, since a target would write it whole at
        // every index otherwise
        auto const is_array = c.out.builtin_type_of(info.type) == nullptr && c.out.at(info.type).kind == type_kind::array;
        if (is_array)
            for (auto const& h : hoisted_constants)
                if (h.symbol == info.symbol)
                    return local_ref(h.local, from);
        auto at = isize(0);
        auto const result = value_of(info.type, from, info.leaves, at);
        if (at != info.leaves.size())
            return fail();
        if (!is_array)
            return result;
        auto const local = add_local(local_kind::let, c.out.at(info.symbol).name, info.type);
        hoisted_lets.push_back(make_stmt({.file = file(), .expr = from}, flat_let{.local = local, .value = result}));
        hoisted_constants.push_back({.symbol = info.symbol, .local = local});
        return local_ref(local, from);
    }
    }
    return fail();
}

auto sgl::check::impl::flattener::value_of(type_id type, ast::expr_id from, cc::span<scalar const> leaves, isize& at)
    -> flat_expr_id
{
    if (auto const* const record = c.out.builtin_type_of(type))
    {
        if (record->leaf_count == 1)
        {
            if (at >= leaves.size())
                return fail();
            auto const leaf = leaves[at++];
            switch (leaf.kind)
            {
            case value_kind::scalar_float:
            case value_kind::scalar_half:
                return add_expr(type, from, flat_literal{.value = f64(leaf.widened().as_float())});
            case value_kind::scalar_int:
            case value_kind::scalar_short:
                return add_expr(type, from, flat_int_literal{.value = leaf.widened().as_int()});
            case value_kind::scalar_uint:
            case value_kind::scalar_ushort:
                return add_expr(type, from, flat_int_literal{.value = leaf.widened().as_int(), .is_unsigned = true});
            case value_kind::boolean:
                return add_expr(type, from, flat_bool_literal{.value = leaf.as_bool()});
            case value_kind::scalar_long:
            case value_kind::scalar_ulong:
                return add_expr(type, from, flat_wide_literal{.bits = leaf.wide_bits()});
            case value_kind::scalar_double:
                return add_expr(type, from, flat_literal{.value = leaf.as_double()});
            default:
                return fail();
            }
        }
        // a vector has its components as fields, and a matrix, a `mat2` among them, has none
        if (c.out.at(type).members.count > 0)
        {
            auto arguments = cc::vector<flat_expr_id>();
            for (auto i = 0; i < record->leaf_count; ++i)
                arguments.push_back(value_of(field_type(type, i), from, leaves, at));
            return add_expr(type, from, flat_construct{.arguments = add_list(arguments)});
        }
        // a matrix is its constructor of columns, `mat4(c0, c1, c2, c3)`, which every target writes as it must
        auto const* const overloads = c.prelude_names.get_ptr(c.out.name_of(type));
        if (overloads == nullptr)
            return fail();
        for (auto const callee : *overloads)
        {
            auto const& s = c.out.at(callee);
            if (s.kind != symbol_kind::function || s.info < 0)
                continue;
            auto const& f = c.out.functions[s.info];
            auto const parameters = c.out.at(f.parameters);
            // the constructor of columns has as many as each column has rows
            if (f.result != type || parameters.size() * parameters.size() != record->leaf_count)
                continue;
            auto columns = cc::vector<flat_expr_id>();
            for (auto const& p : parameters)
                columns.push_back(value_of(p.type, from, leaves, at));
            return builtin_call(from, callee, columns);
        }
        return fail();
    }
    auto const& t = c.out.at(type);
    if (t.kind == type_kind::enumeration)
    {
        if (at >= leaves.size())
            return fail();
        auto const leaf = leaves[at++];
        auto const cases = c.out.at(t.cases);
        for (auto i = isize(0); i < cases.size(); ++i)
            if (cases[i].value == leaf.as_int())
                return enum_value(type, from, i32(i));
        return fail();
    }
    auto arguments = cc::vector<flat_expr_id>();
    if (t.kind == type_kind::array)
        for (auto i = 0; i < t.count; ++i)
            arguments.push_back(value_of(t.element, from, leaves, at));
    else if (t.kind == type_kind::structure)
        for (auto const& member : c.out.at(t.members))
            arguments.push_back(value_of(member.type, from, leaves, at));
    else
        return fail();
    return add_expr(type, from, flat_construct{.arguments = add_list(arguments)});
}

auto sgl::check::impl::flattener::flatten_number(ast::expr_id id, type_id type) -> flat_expr_id
{
    auto text = c.text_of(file(), c.span_of(file(), id));
    // CHK-357: a suffix named the type already, and the value is the number before it
    if (auto const suffixed = split_suffix(text); suffixed.has_value())
        text = suffixed.value().body;
    auto const is_half = type == c.prelude_type(builtins::k_half);
    auto const is_float
        = is_half || type == c.prelude_type(builtins::k_float) || type == c.prelude_type(builtins::k_double);
    // CHK-253: a half literal is the half nearest its value, which every target's text then spells exactly
    auto const literal = [&](f64 v)
    { return add_expr(type, id, flat_literal{.value = is_half ? f64(scalar::of_half(v).widened().as_float()) : v}); };
    if (classify_number(text) == number_class::plain_integer)
    {
        if (auto const past_long = parse_literal_past_long(text);
            past_long.has_value() && type == c.prelude_type(builtins::k_ulong))
            return add_expr(type, id, flat_wide_literal{.bits = past_long.value()});
        auto const value = parse_literal_integer(text);
        if (!value.has_value())
            return fail();
        if (is_float)
            return literal(f64(value.value()));
        // CHK-450: a 64-bit literal keeps all of its bits, two's complement for either type
        if (type == c.prelude_type(builtins::k_long) || type == c.prelude_type(builtins::k_ulong))
            return add_expr(type, id, flat_wide_literal{.bits = u64(value.value())});
        // an unsigned literal keeps its bits in `value`
        auto const is_unsigned = type == c.prelude_type(builtins::k_uint) || type == c.prelude_type(builtins::k_ushort);
        auto const v = value.value();
        if (is_unsigned ? v < 0 || v > 4294967295ll : v < -2147483647 - 1 || v > 2147483647)
            return fail();
        return add_expr(
            type, id, flat_int_literal{.value = i32(u32(v)), .is_unsigned = is_unsigned, .is_hex = is_hex_literal(text)});
    }
    auto const value = parse_plain_float(text);
    return value.has_value() ? literal(value.value()) : fail();
}

auto sgl::check::impl::flattener::flatten_folded(ast::expr_id id, type_id type, number_literal const& n) -> flat_expr_id
{
    auto const is_half = type == c.prelude_type(builtins::k_half);
    auto const is_float
        = is_half || type == c.prelude_type(builtins::k_float) || type == c.prelude_type(builtins::k_double);
    if (n.is_integer && (type == c.prelude_type(builtins::k_long) || type == c.prelude_type(builtins::k_ulong)))
        return add_expr(type, id, flat_wide_literal{.bits = u64(n.integer)});
    if (n.is_integer && !is_float)
    {
        auto const is_unsigned = type == c.prelude_type(builtins::k_uint) || type == c.prelude_type(builtins::k_ushort);
        auto const v = n.integer;
        if (is_unsigned ? v < 0 || v > 4294967295ll : v < -2147483647 - 1 || v > 2147483647)
            return fail();
        return add_expr(type, id, flat_int_literal{.value = i32(u32(v)), .is_unsigned = is_unsigned});
    }
    if (!is_float)
        return fail();
    auto const v = n.is_integer ? f64(n.integer) : n.real;
    return add_expr(type, id, flat_literal{.value = is_half ? f64(scalar::of_half(v).widened().as_float()) : v});
}

auto sgl::check::impl::flattener::flatten_array_literal(ast::expr_id id, type_id type, ast::array const& literal)
    -> flat_expr_id
{
    auto values = cc::vector<flat_expr_id>();
    for (auto const& element : ast().at(literal.elements))
        values.push_back(flatten_expr(element.value));
    // a target may build one in any order, so two with an effect run first, in order
    auto with_effect = 0;
    for (auto const v : values)
        with_effect += calls_impure(v, 0) ? 1 : 0;
    if (with_effect >= 2)
        for (auto& v : values)
            if (calls_impure(v, 0))
            {
                auto const local = add_local(local_kind::temporary, "element", entry.at(v).type);
                add_stmt({.file = file(), .expr = id}, flat_let{.local = local, .value = v});
                v = local_ref(local, id);
            }
    return add_expr(type, id, flat_construct{.arguments = add_list(values)});
}

auto sgl::check::impl::flattener::flatten_filled(ast::expr_id id, type_id type, ast::call const& call) -> flat_expr_id
{
    auto const arguments = ast().at(call.arguments);
    if (arguments.size() != 1 || !is_valid(type))
        return fail();
    auto const where = origin{.file = file(), .expr = id};
    auto const value = flatten_expr(arguments[0].value);
    if (!is_valid(value))
        return fail();
    auto const value_type = entry.at(value).type;

    auto const label = add_label("filled");
    auto body = cc::vector<flat_stmt_id>();
    auto filler = value;
    if (!is_substitutable(value))
    {
        auto const held = add_local(local_kind::let, "fill", value_type);
        body.push_back(make_stmt(where, flat_let{.local = held, .value = value}));
        filler = local_ref(held, id);
    }
    auto const result = add_local(local_kind::var, "filled", type);
    body.push_back(make_stmt(where, flat_var{.local = result}));
    // EMIT-231: a small array is filled one constant index at a time, so a target keeps it in registers
    constexpr auto max_unrolled_fill = isize(64);
    auto elements = isize(1);
    for (auto t = type; t != value_type && elements <= max_unrolled_fill; t = c.out.at(t).element)
        elements *= isize(c.out.at(t).count);
    auto const is_unrolled = elements <= max_unrolled_fill;

    // appends the statements that assign `filler` to every element of `place`, of type `t`, down to the filler's own type
    auto const fill = [&](auto const& self, flat_expr_id place, type_id t, cc::vector<flat_stmt_id>& out) -> void
    {
        if (t == value_type)
        {
            out.push_back(make_stmt(where, flat_assign{.place = place, .value = filler}));
            return;
        }
        auto const& info = c.out.at(t);
        if (is_unrolled)
        {
            for (auto k = 0; k < info.count; ++k)
            {
                auto const at = add_expr(int_type(), id, flat_int_literal{.value = k});
                self(self, add_expr(info.element, id, flat_element{.object = place, .index = at}), info.element, out);
            }
            return;
        }
        auto const index = add_local(local_kind::index, "i", int_type());
        auto const element = add_expr(info.element, id, flat_element{.object = place, .index = local_ref(index, id)});
        auto store = cc::vector<flat_stmt_id>();
        self(self, element, info.element, store);
        out.push_back(make_stmt(where, flat_for{.label = add_label("fill"),
                                                .index = index,
                                                .first = add_expr(int_type(), id, flat_int_literal{.value = 0}),
                                                .end = add_expr(int_type(), id, flat_int_literal{.value = info.count}),
                                                .body = add_list(store)}));
    };
    fill(fill, local_ref(result, id), type, body);
    body.push_back(make_stmt(where, flat_leave{.target = label, .value = local_ref(result, id)}));
    return add_expr(type, id, flat_block{.label = label, .body = add_list(body)});
}

auto sgl::check::impl::flattener::flatten_call(ast::expr_id id, type_id type, target const& where, ast::call const& call)
    -> flat_expr_id
{
    if (where.kind == target_kind::array_filled)
        return flatten_filled(id, type, call);
    // CHK-341: `undefined()` is a local declared and never assigned, whose value nobody reads
    if (where.kind == target_kind::undefined_value)
    {
        auto const local = add_local(local_kind::var, "undefined", type);
        entry.locals[index_of(local)].is_undefined = true;
        add_stmt({.file = file(), .expr = id}, flat_var{.local = local});
        return local_ref(local, id);
    }
    if (sgl::is_valid(call.op))
    {
        auto const spelling = c.text_of(file(), c.file_of(file()).at(call.op).where);
        auto const arguments = ast().at(call.arguments);
        if (spelling == "not" && arguments.size() == 1)
            return add_expr(type, id, flat_not{.operand = flatten_expr(arguments[0].value)});
        if (call.is_short_circuit && arguments.size() == 2)
        {
            auto const lhs = flatten_expr(arguments[0].value);
            auto const rhs = flatten_expr(arguments[1].value);
            return spelling == "and" ? add_expr(type, id, flat_and{.lhs = lhs, .rhs = rhs})
                                     : add_expr(type, id, flat_or{.lhs = lhs, .rhs = rhs});
        }
        if (spelling == "not" || call.is_short_circuit)
            return fail();
        if ((spelling == "==" || spelling == "!=") && arguments.size() == 2
            && tables().type_at(arguments[0].value) == checked_module::void_type
            && tables().type_at(arguments[1].value) == checked_module::void_type)
            return flatten_void_equality(id, type, spelling == "==", arguments[0].value, arguments[1].value);
        if ((spelling == "==" || spelling == "!=") && arguments.size() == 2 && where.kind != target_kind::overload
            && c.out.at(concrete(tables().type_at(arguments[0].value))).kind == type_kind::array)
        {
            auto const lhs = flatten_expr(arguments[0].value);
            auto const rhs = flatten_expr(arguments[1].value);
            return flatten_array_equality(id, type, spelling == "==", lhs, rhs);
        }
    }

    for (auto const& t : c.out.ray_traces)
        if (t.call == id && t.file == file())
            return flatten_ray_trace(id, call, t);
    for (auto const& t : c.out.callable_calls)
        if (t.call == id && t.file == file())
            return flatten_callable_call(id, call, t);

    auto const record = tables().call_at(id);
    // CHK-319: a call through a parameter of function type records no callee, and inlines what was handed over
    if (record >= 0 && !is_valid(c.out.call_records[record].callee) && ast::is_valid(call.callee))
    {
        auto const through = tables().target_at(call.callee);
        for (auto const& b : current()->bound)
            if (b.where == through && b.closure >= 0)
                return flatten_closure_call(id, type, b.closure, c.out.call_records[record]);
        return fail();
    }
    if (record < 0 || (where.kind != target_kind::overload && where.kind != target_kind::constructor))
        return fail();
    return flatten_bound_call(id, type, c.out.call_records[record]);
}

auto sgl::check::impl::flattener::flatten_ray_trace(ast::expr_id id, ast::call const& call, ray_trace const& t)
    -> flat_expr_id
{
    auto const arguments = ast().at(call.arguments);
    auto const where = origin{.file = file(), .expr = id};
    if (t.kind == ray_trace_kind::invoke)
        return flatten_hit_invoke(id, call);
    auto const world = flatten_expr(arguments[0].value);
    auto const world_type = tables().type_at(arguments[0].value);
    // CHK-434: a trace_hit gives the hit object, and runs no shader but the traversal's
    auto const is_hit_object = t.kind == ray_trace_kind::hit_object;
    auto const* const overloads = c.prelude_names.get_ptr(cc::string_view(is_hit_object ? "trace_hit_ray" : "trace_ray"));
    auto callee = symbol_id::none;
    for (auto const candidate :
         overloads == nullptr ? cc::span<symbol_id const>() : cc::span<symbol_id const>(*overloads))
        if (c.out.at(c.out.functions[c.out.at(candidate).info].parameters)[0].type == world_type)
            callee = candidate;
    if (!is_valid(callee) || !is_valid(world))
        return fail();

    // the ray once, then its fields
    auto ray = flatten_expr(arguments[1].value);
    if (!is_valid(ray))
        return fail();
    if (!is_substitutable(ray))
    {
        auto const local = add_local(local_kind::let, "ray", entry.at(ray).type);
        add_stmt(where, flat_let{.local = local, .value = ray});
        ray = local_ref(local, id);
    }
    auto const fields = c.out.at(c.out.at(entry.at(ray).type).members);
    auto const field
        = [&](i32 m) { return add_expr(fields[m].type, id, flat_member{.object = again(ray, id), .member = m}); };

    auto flags = add_expr(c.out.at(c.out.functions[c.out.at(callee).info].parameters)[5].type, id,
                          flat_int_literal{.value = 0});
    auto mask = add_expr(int_type(), id, flat_int_literal{.value = 0xff});
    auto reorder = ast::expr_id::none;
    auto reorder_bits = ast::expr_id::none;
    for (auto i = isize(4); i < arguments.size(); ++i)
    {
        auto const name = c.text_of(file(), arguments[i].name);
        if (name == "reorder")
            reorder = arguments[i].value;
        else if (name == "reorder_bits")
            reorder_bits = arguments[i].value;
        else
            (name == "flags" ? flags : mask) = flatten_expr(arguments[i].value);
    }
    // CHK-433: a raygen of a dx12 or vulkan compile whose device reorders traces through a hit object; every other
    // trace is the plain one, its hint unevaluated, since a hint changes nothing a shader computes
    auto const target = compiled_for();
    auto const may_reorder = ast::is_valid(reorder) && entry.entry_stage == stage::raygen
                          && (target.api == graphics_api::dx12 || target.api == graphics_api::vulkan);
    if (may_reorder)
        entry.consulted.set(feature::shader_execution_reordering); // EMIT-264
    auto const reorders = may_reorder && target.device_features.has(feature::shader_execution_reordering);
    auto hint = flat_expr_id::none;
    auto hint_bits = flat_expr_id::none;
    auto is_reordered = false;
    if (reorders && c.out.name_of(tables().type_at(reorder)) == builtins::k_bool)
        is_reordered = c.known_condition(file(), reorder).value_or(false);
    else if (reorders)
    {
        hint = flatten_expr(reorder);
        hint_bits = flatten_expr(reorder_bits);
        is_reordered = is_valid(hint) && is_valid(hint_bits);
    }
    if (is_reordered)
    {
        auto const name = cc::string_view(is_valid(hint) ? "trace_ray_reordered_by_hint" : "trace_ray_reordered");
        auto const* const reordered = c.prelude_names.get_ptr(name);
        for (auto const candidate :
             reordered == nullptr ? cc::span<symbol_id const>() : cc::span<symbol_id const>(*reordered))
            if (c.out.at(c.out.functions[c.out.at(candidate).info].parameters)[0].type == world_type)
                callee = candidate;
        auto within = cc::vector<symbol_id>();
        for (auto const& fr : frames)
            within.push_back(fr.function);
        auto const used = feature_set(feature::shader_execution_reordering);
        feature_uses.push_back(
            {.file = file(), .call = id, .features = used, .within = cc::move(within), .granted = used});
    }
    auto const count = c.out.at(c.out.at(c.out.at(t.set).type).members).size();

    auto place = flatten_expr(arguments[3].value);
    if (!is_valid(place))
        return fail();
    auto const is_local = entry.at(place).node.is<flat_local_ref>();
    auto payload = place;
    if (!is_local)
    {
        place = with_bound_index(place, id);
        auto const local = add_local(local_kind::var, "payload", entry.at(place).type);
        add_stmt(where, flat_var{.local = local});
        add_stmt(where, flat_assign{.place = add_expr(entry.at(place).type, id, flat_local_ref{.local = local}),
                                    .value = again(place, id)});
        payload = local_ref(local, id);
    }

    auto values = cc::vector<flat_expr_id>{
        world,
        field(0),
        field(1),
        field(2),
        field(3),
        flags,
        mask,
        add_expr(int_type(), id, flat_int_literal{.value = t.ray}),
        add_expr(int_type(), id, flat_int_literal{.value = i32(count)}),
        add_expr(int_type(), id, flat_int_literal{.value = t.ray}),
    };
    if (is_reordered && is_valid(hint))
    {
        values.push_back(hint);
        values.push_back(hint_bits);
    }
    // the ray's time, past the signature ahead of the payload, which stays last: MSL places what moves at it
    values.push_back(field(4));
    values.push_back(payload);
    auto const traced_ray = flat_traced_ray{.set = t.set, .ray = t.ray};
    if (!entry.traced_rays.values().any([&](flat_traced_ray const& r) { return r == traced_ray; }))
        entry.traced_rays.push_back(traced_ray);
    auto const traced = builtin_call(id, callee, values);
    if (is_local)
        return traced;
    if (is_hit_object)
    {
        auto const hit = add_local(local_kind::let, "hit", entry.at(traced).type);
        add_stmt(where, flat_let{.local = hit, .value = traced});
        add_stmt(where, flat_assign{.place = again(place, id), .value = again(payload, id)});
        return local_ref(hit, id);
    }
    add_stmt(where, flat_eval{.value = traced});
    add_stmt(where, flat_assign{.place = again(place, id), .value = again(payload, id)});
    return add_expr(checked_module::void_type, id, flat_construct{});
}

auto sgl::check::impl::flattener::flatten_hit_invoke(ast::expr_id id, ast::call const& call) -> flat_expr_id
{
    auto const where = origin{.file = file(), .expr = id};
    auto const& member = ast().at(call.callee).node.as<ast::member>();
    auto const arguments = ast().at(call.arguments);
    auto const* const overloads = c.prelude_names.get_ptr("hit_object_invoke");
    auto const hit = flatten_expr(member.object);
    if (overloads == nullptr || overloads->empty() || !is_valid(hit))
        return fail();

    // the payload a trace hands over, copied through a local where it is no local itself
    auto place = flatten_expr(arguments[0].value);
    if (!is_valid(place))
        return fail();
    auto const is_local = entry.at(place).node.is<flat_local_ref>();
    auto payload = place;
    if (!is_local)
    {
        place = with_bound_index(place, id);
        auto const local = add_local(local_kind::var, "payload", entry.at(place).type);
        add_stmt(where, flat_var{.local = local});
        add_stmt(where, flat_assign{.place = add_expr(entry.at(place).type, id, flat_local_ref{.local = local}),
                                    .value = again(place, id)});
        payload = local_ref(local, id);
    }
    auto const invoked = builtin_call(id, overloads->front(), cc::vector<flat_expr_id>{hit, payload});
    if (is_local)
        return invoked;
    add_stmt(where, flat_eval{.value = invoked});
    add_stmt(where, flat_assign{.place = again(place, id), .value = again(payload, id)});
    return add_expr(checked_module::void_type, id, flat_construct{});
}

auto sgl::check::impl::flattener::decide_report(origin from, flat_expr_id reported) -> flat_expr_id
{
    auto const type = entry.at(reported).type;
    auto const id = from.expr;
    auto const members = c.out.at(c.out.at(type).members);
    auto member_at = [&](cc::string_view name)
    {
        for (auto i = isize(0); i < members.size(); ++i)
            if (members[i].name == name)
                return i32(i);
        return i32(-1);
    };
    auto const is_hit = member_at("is_hit");
    auto const t = member_at("t");
    auto const* const t_min = c.prelude_names.get_ptr("ray_t_min");
    auto const* const t_current = c.prelude_names.get_ptr("ray_t_current");
    auto const* const candidate_of = c.prelude_names.get_ptr("candidate_of_box");
    if (is_hit < 0 || t < 0 || t_min == nullptr || t_current == nullptr || candidate_of == nullptr)
        return fail();
    type_id const floats[] = {c.prelude_type(builtins::k_float), c.prelude_type(builtins::k_float)};
    auto const at_least = c.find_operator(file(), ">=", floats);
    auto const at_most = c.find_operator(file(), "<=", floats);
    if (!is_valid(at_least) || !is_valid(at_most))
        return fail();

    auto const r = add_local(local_kind::var, "reported", type);
    add_stmt(from, flat_var{.local = r, .value = reported});
    auto const field
        = [&](i32 m) { return add_expr(members[m].type, id, flat_member{.object = local_ref(r, id), .member = m}); };
    auto const no_hit = [&]
    {
        return make_stmt(from,
                         flat_assign{.place = field(is_hit),
                                     .value = add_expr(members[is_hit].type, id, flat_bool_literal{.value = false})});
    };

    flat_expr_id const low[] = {field(t), builtin_call(id, t_min->front(), {})};
    flat_expr_id const high[] = {field(t), builtin_call(id, t_current->front(), {})};
    auto const in_range
        = add_expr(members[is_hit].type, id,
                   flat_and{.lhs = builtin_call(id, at_least, low), .rhs = builtin_call(id, at_most, high)});

    auto const outer = cc::move(block);
    block = {};
    if (is_valid(fused.value().any_hit))
    {
        // the candidate, as raytracing.sgl builds it from the box and the report
        auto const attributes = c.out.at(type).element;
        auto const builder = candidate_of->front();
        type_id const bindings[] = {c.out.at(c.out.functions[c.out.at(builder).info].type_parameters)[0], attributes};
        flat_expr_id const box_and_report[] = {local_ref(fused.value().box, id), local_ref(r, id)};
        i32 const both[] = {0, 1};
        auto const made = inline_bound(id, builder, box_and_report, both, {}, bindings);
        auto const& info = c.out.functions[c.out.at(builder).info];
        auto const candidate_type = c.substitute_existing(info.result, bindings);
        auto const candidate = add_expr(candidate_type, id, flat_block{.label = made.label, .body = made.body});

        flat_expr_id const handed[] = {candidate, local_ref(fused.value().payload, id)};
        auto const decided = inline_bound(id, fused.value().any_hit, handed, both);
        auto const decision_type = c.out.functions[c.out.at(fused.value().any_hit).info].result;
        auto const decision = add_local(local_kind::let, "decision", decision_type);
        add_stmt(from, flat_let{.local = decision,
                                .value = add_expr(decision_type, id,
                                                  flat_block{.label = decided.label, .body = decided.body})});
        // `==` of one enum is the `int` overload (CHK-149)
        type_id const ints[] = {c.prelude_type(builtins::k_int), c.prelude_type(builtins::k_int)};
        auto const equals = c.find_operator(file(), "==", ints);
        if (!is_valid(equals))
            return fail();
        // `ignore` is the decision's case 1, which no report survives
        flat_expr_id const compared[]
            = {local_ref(decision, id), add_expr(decision_type, id, flat_enum_value{.case_index = 1})};
        flat_stmt_id const ignored[] = {no_hit()};
        add_stmt(from, flat_if{.condition = builtin_call(id, equals, compared), .then_body = add_list(ignored)});
        // `accept_and_end_search` is case 2, which the traversal's result carries out as metal's `continue_search`
        auto const ends = fused.value().ends_search;
        flat_expr_id const ending[]
            = {local_ref(decision, id), add_expr(decision_type, id, flat_enum_value{.case_index = 2})};
        flat_stmt_id const ended[] = {make_stmt(
            from, flat_assign{.place = local_ref(ends, id),
                              .value = add_expr(members[is_hit].type, id, flat_bool_literal{.value = true})})};
        add_stmt(from, flat_if{.condition = builtin_call(id, equals, ending), .then_body = add_list(ended)});
    }
    auto const decide = add_list(block);
    block = cc::move(outer);
    flat_stmt_id const out_of_range[] = {no_hit()};
    flat_stmt_id const checked[]
        = {make_stmt(from, flat_if{.condition = in_range, .then_body = decide, .else_body = add_list(out_of_range)})};
    add_stmt(from, flat_if{.condition = field(is_hit), .then_body = add_list(checked)});
    return local_ref(r, id);
}

auto sgl::check::impl::flattener::flatten_callable_call(ast::expr_id id, ast::call const& call, callable_call const& t)
    -> flat_expr_id
{
    auto const where = origin{.file = file(), .expr = id};
    auto base = 0;
    for (auto const& p : c.out.pipelines)
        if (p.kind == pipeline_kind::callables && index_of(p.symbol) < index_of(t.table))
            base += i32(p.records.count);
    auto const* const found = c.prelude_names.get_ptr("call_callable");
    if (found == nullptr || found->empty())
        return fail();
    auto const callee = found->front();

    auto const& index_node = ast().at(call.callee).node.as<ast::index>();
    auto index = flatten_expr(ast().at(index_node.arguments)[0].value);
    if (!is_valid(index))
        return fail();
    if (base != 0)
    {
        type_id const types[] = {int_type(), int_type()};
        auto const plus = c.find_operator(file(), "+", types);
        if (!is_valid(plus))
            return fail();
        flat_expr_id const operands[] = {index, add_expr(int_type(), id, flat_int_literal{.value = base})};
        index = builtin_call(id, plus, operands);
    }

    auto place = flatten_expr(ast().at(call.arguments)[0].value);
    if (!is_valid(place))
        return fail();
    auto const is_local = entry.at(place).node.is<flat_local_ref>();
    auto parameter = place;
    if (!is_local)
    {
        place = with_bound_index(place, id);
        auto const local = add_local(local_kind::var, "parameter", entry.at(place).type);
        add_stmt(where, flat_var{.local = local});
        add_stmt(where, flat_assign{.place = add_expr(entry.at(place).type, id, flat_local_ref{.local = local}),
                                    .value = again(place, id)});
        parameter = local_ref(local, id);
    }
    flat_expr_id const values[] = {index, parameter};
    auto const called = builtin_call(id, callee, values);
    if (is_local)
        return called;
    add_stmt(where, flat_eval{.value = called});
    add_stmt(where, flat_assign{.place = again(place, id), .value = again(parameter, id)});
    return add_expr(checked_module::void_type, id, flat_construct{});
}

auto sgl::check::impl::flattener::with_bound_index(flat_expr_id place, ast::expr_id id) -> flat_expr_id
{
    // by value: binding adds nodes, and the arrays move
    auto const x = entry.at(place);
    auto bind = [&](flat_expr_id index)
    { return bound_index(index, local_kind::temporary, "index", {.file = file(), .expr = id}); };
    if (auto const* const element = x.node.try_as<flat_buffer_element>())
    {
        auto const buffer = element->buffer;
        auto const index = bind(element->index);
        return add_expr(x.type, id, flat_buffer_element{.buffer = buffer, .index = index});
    }
    if (auto const* const element = x.node.try_as<flat_element>())
    {
        auto const object = element->object;
        auto const index = bind(element->index);
        return add_expr(x.type, id, flat_element{.object = object, .index = index});
    }
    return place;
}

auto sgl::check::impl::flattener::is_inlined(symbol_id callee) const -> bool
{
    auto const& s = c.out.at(callee);
    return s.role != function_role::constructor && !is_valid(s.intrinsic);
}

auto sgl::check::impl::flattener::flatten_bound_call(ast::expr_id id, type_id type, call_record const& record)
    -> flat_expr_id
{
    if (is_by_target(record.callee))
        return flatten_by_target(id, type, record);
    // CHK-426: the device this compile writes for answers `has`
    if (is_has_call(record.callee))
        return flatten_has(id, type, record);
    if (is_diff_call(record.callee))
        return flatten_diff(id, type, record);
    if (auto const texel = atomic_texel_of(record); ast::is_valid(texel))
        return flatten_texel_atomic(id, type, record, texel);
    auto const values = flatten_written(c.out.at(record.written));
    auto const handed = written_closures;
    auto const slots = c.out.at(record.slots);
    if (is_inlined(record.callee))
    {
        if (diff_depth > 0 && type != checked_module::void_type)
            if (auto const d = custom_derivatives_of(record.callee); is_valid(d.forward) || is_valid(d.backward))
                return flatten_custom_call(id, type, record, d, values);
        auto const inlined = inline_bound(id, record.callee, values, slots, handed, c.out.at(record.type_arguments));
        if (diff_depth > 0 && is_detach_call(record.callee) && is_valid(inlined.label))
            custom_calls[i32(inlined.label)] = {.function = record.callee, .call = id, .is_detached = true};
        return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
    }
    return target_call(id, type, record.callee, values, slots);
}

auto sgl::check::impl::flattener::atomic_texel_of(call_record const& record) const -> ast::expr_id
{
    auto const written = c.out.at(record.written);
    if (written.empty())
        return ast::expr_id::none;
    auto const* const indexed = ast().at(written[0].expr).node.try_as<ast::index>();
    if (indexed == nullptr)
        return ast::expr_id::none;
    auto const image = tables().type_at(indexed->object);
    return is_valid(image) && c.out.at(image).kind == type_kind::image && c.out.at(image).is_atomic ? written[0].expr
                                                                                                    : ast::expr_id::none;
}

auto sgl::check::impl::flattener::flatten_texel_atomic(ast::expr_id id,
                                                       type_id type,
                                                       call_record const& record,
                                                       ast::expr_id texel) -> flat_expr_id
{
    auto const* const atomic = c.out.builtin_function(c.out.at(record.callee).intrinsic);
    auto const located = tables().call_at(texel);
    if (atomic == nullptr || located < 0)
        return fail();
    auto const& image = c.out.at(tables().type_at(ast().at(texel).node.as<ast::index>().object));
    auto const callee = texel_atomic(cc::format("texel_{}", atomic->name), image);
    if (!is_valid(callee))
        return fail();
    // the internal atomic takes what the image's `load` does, in the same order, and the operands after it
    auto const& load = c.out.call_records[located];
    auto values = flatten_written(c.out.at(load.written));
    auto slots = cc::vector<i32>::create_copy_of(c.out.at(load.slots));
    auto const written = c.out.at(record.written);
    for (auto i = isize(1); i < written.size(); ++i)
    {
        slots.push_back(i32(values.size()));
        values.push_back(flatten_expr(written[i].expr));
    }
    return target_call(id, type, callee, values, slots);
}

auto sgl::check::impl::flattener::texel_atomic(cc::string_view name, check::type_info const& image) const -> symbol_id
{
    auto const* const found = c.prelude_names.get_ptr(cc::string(name));
    if (found == nullptr)
        return symbol_id::none;
    auto const texel = texel_name_of(image.format);
    for (auto const f : *found)
    {
        auto const& s = c.out.at(f);
        if (s.kind != symbol_kind::function || s.info < 0)
            continue;
        auto const parameters = c.out.at(c.out.functions[s.info].parameters);
        if (parameters.empty())
            continue;
        auto const& p = c.out.at(parameters[0].type);
        if (p.kind == type_kind::image && p.shape == image.shape && c.out.name_of(p.element) == texel)
            return f;
    }
    return symbol_id::none;
}

auto sgl::check::impl::flattener::target_call(ast::expr_id id,
                                              type_id type,
                                              symbol_id callee,
                                              cc::span<flat_expr_id const> values,
                                              cc::span<i32 const> slots) -> flat_expr_id
{
    auto const& s = c.out.at(callee);
    auto in_order = slots.size() == values.size();
    for (auto p = isize(0); in_order && p < slots.size(); ++p)
        in_order = slots[p] == p;
    auto with_effect = 0;
    for (auto const v : values)
        with_effect += calls_impure(v, 0) ? 1 : 0;

    auto const call_of = [&](cc::span<flat_expr_id const> arguments)
    {
        if (s.role == function_role::constructor)
            return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
        return builtin_call(id, callee, arguments);
    };
    if (in_order && with_effect < 2)
        return call_of(values);

    auto const parameters = c.out.at(c.out.functions[s.info].parameters);
    auto const where = origin{.file = file(), .expr = id};
    auto const label = add_label(s.name);
    auto const outer = cc::move(block);
    block = {};
    auto bound = cc::vector<flat_expr_id>::create_filled(values.size(), flat_expr_id::none);
    for (auto i = isize(0); i < values.size(); ++i)
    {
        auto const value = values[i];
        if (!is_valid(value))
            return fail();
        if (is_substitutable(value) || is_resource_member(value) || is_literal_construction(value))
        {
            bound[i] = value;
            continue;
        }
        // an atomic names the memory the call updates, and a binding array's element the resource it hands over,
        // so either's index is bound in its stead, and never its value
        if (auto const kind = c.out.at(entry.at(value).type).kind; kind == type_kind::atomic || is_resource(kind))
        {
            bound[i] = with_bound_index(value, id);
            continue;
        }
        auto name = cc::string_view("argument");
        for (auto p = isize(0); p < slots.size(); ++p)
            if (slots[p] == i)
                name = parameters[p].name;
        auto const local = add_local(local_kind::let, name, entry.at(value).type);
        add_stmt(where, flat_let{.local = local, .value = value});
        bound[i] = local_ref(local, id);
    }
    // The defaults are written in the callee's frame, which reads its parameters as the call filled them.
    auto has_default = false;
    for (auto const slot : slots)
        has_default = has_default || slot < 0;
    auto filled = cc::vector<bound_name>();
    if (has_default)
    {
        // the copies are made in the caller's frame, where `id` stands, before the callee's frame is pushed
        auto copies = cc::vector<flat_expr_id>();
        for (auto p = isize(0); p < slots.size(); ++p)
            copies.push_back(slots[p] >= 0 ? again(bound[slots[p]], id) : flat_expr_id::none);
        auto const chain = chain_through(id);
        raise_float_level(callee);
        frames.push_back({.function = callee, .file = s.file, .chain = chain});
        for (auto p = isize(0); p < slots.size(); ++p)
            if (slots[p] >= 0)
                bind_parameter(parameters[p], copies[p], true);
        bind_defaults(parameters, slots);
        filled = cc::move(current()->bound);
        frames.remove_back();
    }

    auto arguments = cc::vector<flat_expr_id>();
    for (auto p = isize(0); p < slots.size(); ++p)
    {
        if (slots[p] >= 0)
        {
            auto const value = bound[slots[p]];
            arguments.push_back(entry.at(value).node.is<flat_local_ref>() || p == 0 ? value : again(value, id));
            continue;
        }
        auto const where_p = target{.kind = target_kind::parameter, .index = i32(parameters[p].field)};
        auto value = flat_expr_id::none;
        for (auto const& b : filled)
            if (b.where == where_p)
                value = is_valid(b.literal) ? again(b.literal, id) : local_ref(b.local, id);
        if (!is_valid(value))
            return fail();
        arguments.push_back(value);
    }
    // CHK-303: a stream's `emit` takes the vertex past its signature, which no slot names
    if (auto const* const record = c.out.builtin_function(s.intrinsic); record != nullptr && record->takes_element)
        for (auto i = slots.size(); i < bound.size(); ++i)
            arguments.push_back(entry.at(bound[i]).node.is<flat_local_ref>() ? bound[i] : again(bound[i], id));
    add_stmt(where, flat_leave{.target = label, .value = call_of(arguments)});
    auto const body = add_list(block);
    block = cc::move(outer);
    return add_expr(type, id, flat_block{.label = label, .body = body});
}

auto sgl::check::impl::flattener::calls_impure(flat_expr_id id, int depth) const -> bool
{
    if (!is_valid(id) || depth > k_max_inline_depth)
        return false;
    auto const& x = entry.at(id);
    if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && !call->is_pure)
        return true;
    if (auto const* const block = x.node.try_as<flat_block>())
        return writes_outside(entry, block->body, depth + 1);
    auto result = false;
    for_each_operand(entry, x, [&](flat_expr_id operand) { result = result || calls_impure(operand, depth + 1); });
    return result;
}

auto sgl::check::impl::flattener::flatten_void_equality(ast::expr_id id,
                                                        type_id type,
                                                        bool is_equal,
                                                        ast::expr_id lhs,
                                                        ast::expr_id rhs) -> flat_expr_id
{
    auto const label = add_label("void_equality");
    auto const where = origin{.file = file(), .expr = id};
    auto const left = flatten_expr(lhs);
    auto const right = flatten_expr(rhs);
    flat_stmt_id const body[] = {
        make_stmt(where, flat_eval{.value = left}),
        make_stmt(where, flat_eval{.value = right}),
        make_stmt(where, flat_leave{.target = label, .value = add_expr(type, id, flat_bool_literal{.value = is_equal})}),
    };
    return add_expr(type, id, flat_block{.label = label, .body = add_list(body)});
}

auto sgl::check::impl::flattener::flatten_array_equality(ast::expr_id id,
                                                         type_id type,
                                                         bool is_equal,
                                                         flat_expr_id lhs,
                                                         flat_expr_id rhs) -> flat_expr_id
{
    auto const label = add_label("array_equality");
    auto const where = origin{.file = file(), .expr = id};
    auto const array = entry.at(lhs).type;
    auto const element = c.out.at(array).element;
    auto const left = add_local(local_kind::temporary, "left", array);
    auto const right = add_local(local_kind::temporary, "right", array);
    auto const index = add_local(local_kind::index, "i", int_type());
    auto const element_of = [&](local_id of)
    { return add_expr(element, id, flat_element{.object = local_ref(of, id), .index = local_ref(index, id)}); };

    auto is_same = flat_expr_id::none;
    if (c.out.at(element).kind == type_kind::array)
        is_same = flatten_array_equality(id, type, true, element_of(left), element_of(right));
    else
    {
        // an enum compares as the `int` its cases are (EVAL-64)
        auto const compared = c.out.is_plain_enum(element) ? int_type() : element;
        type_id const both[] = {compared, compared};
        auto const equals = c.find_operator(file(), "==", both);
        if (!is_valid(equals))
            return fail();
        flat_expr_id const elements[] = {element_of(left), element_of(right)};
        is_same = builtin_call(id, equals, elements);
    }

    auto const answer = [&](bool value)
    {
        return make_stmt(where,
                         flat_leave{.target = label, .value = add_expr(type, id, flat_bool_literal{.value = value})});
    };
    flat_stmt_id const differs[] = {answer(!is_equal)};
    flat_stmt_id const loop_body[] = {
        make_stmt(where, flat_if{.condition = add_expr(type, id, flat_not{.operand = is_same}),
                                 .then_body = add_list(differs)}),
    };
    flat_stmt_id const body[] = {
        make_stmt(where, flat_let{.local = left, .value = lhs}),
        make_stmt(where, flat_let{.local = right, .value = rhs}),
        make_stmt(where, flat_for{.label = add_label("each_element"),
                                  .index = index,
                                  .first = add_expr(int_type(), id, flat_int_literal{.value = 0}),
                                  .end = add_expr(int_type(), id, flat_int_literal{.value = c.out.at(array).count}),
                                  .body = add_list(loop_body)}),
        answer(is_equal),
    };
    return add_expr(type, id, flat_block{.label = label, .body = add_list(body)});
}

auto sgl::check::impl::flattener::builtin_call(ast::expr_id id, symbol_id callee, cc::span<flat_expr_id const> arguments)
    -> flat_expr_id
{
    judge_stage(id, callee);
    auto const& s = c.out.at(callee);
    if (!is_valid(s.intrinsic) || s.info < 0)
        return fail();
    auto const& info = c.out.functions[s.info];
    auto const* const record = c.out.builtin_function(s.intrinsic);
    if (record != nullptr && record->with_default_sampler != builtin_id::none)
        return default_sampled_call(id, record->with_default_sampler, arguments);
    if (record != nullptr && record->takes_acceleration_index)
    {
        // CHK-325: the position of the member among the entry point's acceleration members, counted in list order
        auto const* const member = !arguments.empty() && is_valid(arguments[0])
                                     ? entry.at(arguments[0]).node.try_as<flat_binding_member>()
                                     : nullptr;
        if (member == nullptr)
            return fail();
        auto k = 0;
        auto is_found = false;
        for (auto const binding : entry.bindings)
        {
            auto const members = c.out.at(c.out.bindings[c.out.at(binding).info].members);
            for (auto i = isize(0); i < members.size() && !is_found; ++i)
            {
                if (binding == member->binding && i32(i) == member->member)
                    is_found = true;
                else if (c.out.at(members[i].type).kind == type_kind::acceleration_structure)
                    ++k;
            }
            if (is_found)
                break;
        }
        if (!is_found)
            return fail();
        auto widened = cc::vector<flat_expr_id>::create_copy_of(arguments);
        widened.push_back(add_expr(int_type(), id, flat_int_literal{.value = k}));
        return add_expr(
            info.result, id,
            flat_call{.callee = callee, .intrinsic = s.intrinsic, .is_pure = info.is_pure, .arguments = add_list(widened)});
    }
    return add_expr(
        info.result, id,
        flat_call{.callee = callee, .intrinsic = s.intrinsic, .is_pure = info.is_pure, .arguments = add_list(arguments)});
}

auto sgl::check::impl::flattener::default_sampled_call(ast::expr_id id,
                                                       builtin_id with_sampler,
                                                       cc::span<flat_expr_id const> arguments) -> flat_expr_id
{
    // the texture, or the binding array it is an element of
    auto named = arguments.size() >= 2 ? arguments[0] : flat_expr_id::none;
    if (auto const* const element = is_valid(named) ? entry.at(named).node.try_as<flat_element>() : nullptr)
        named = element->object;
    auto const* const texture = is_valid(named) ? entry.at(named).node.try_as<flat_binding_member>() : nullptr;
    if (texture == nullptr)
        return fail();
    auto const members = c.out.at(c.out.bindings[c.out.at(texture->binding).info].members);
    auto const& m = members[texture->member];
    auto const file_sampler = m.default_file_sampler;
    if (m.default_sampler < 0 && (!is_valid(file_sampler) || c.out.at(file_sampler).state != symbol_state::checked))
        return fail();
    // the call becomes one of the record that takes the sampler, so its arguments match its callee's parameters
    auto const declared = c.symbol_declaring(with_sampler);
    if (!is_valid(declared))
        return fail();
    auto with = cc::vector<flat_expr_id>();
    with.push_back_range(arguments);
    with.insert_at(2, is_valid(file_sampler)
                          ? add_expr(c.out.at(file_sampler).type, id, flat_file_sampler{.sampler = file_sampler})
                          : add_expr(members[m.default_sampler].type, id,
                                     flat_binding_member{.binding = texture->binding, .member = m.default_sampler}));
    auto const& info = c.out.functions[c.out.at(declared).info];
    return add_expr(
        info.result, id,
        flat_call{.callee = declared, .intrinsic = with_sampler, .is_pure = info.is_pure, .arguments = add_list(with)});
}

auto sgl::check::impl::flattener::flatten_written(cc::span<written_argument const> written) -> cc::vector<flat_expr_id>
{
    auto result = cc::vector<flat_expr_id>();
    auto handed = cc::vector<i32>();
    auto splat = evaluated_once{};
    auto splat_letters = swizzle();
    for (auto const& w : written)
    {
        handed.push_back(w.is_function ? closure_of(w.expr) : -1);
        if (w.is_function)
        {
            // a function is no value: it is handed over as a closure, and the slot holds nothing
            result.push_back(flat_expr_id::none);
            continue;
        }
        if (w.splat_member < 0)
        {
            // CHK-421: a plain vector's constructor converts a scalar of another kind as `as` would
            auto value = flatten_expr(w.expr);
            if (is_valid(w.conversion))
            {
                flat_expr_id const converted[] = {value};
                value = builtin_call(w.expr, w.conversion, converted);
            }
            result.push_back(value);
            continue;
        }
        if (w.splat_member == 0)
        {
            // EMIT-142: a splat of a swizzle reads its operand's fields, and binds no vector of them
            splat_letters = {};
            auto read = w.expr;
            if (is_swizzled(w.expr))
            {
                auto const s = swizzled_at(w.expr);
                read = s.operand;
                splat_letters = s.letters;
            }
            auto const value = flatten_expr(read);
            auto const is_local = is_valid(value) && entry.at(value).node.is<flat_local_ref>();
            splat = !is_valid(value) || is_local ? evaluated_once{.first = value, .later = value}
                                                 : evaluate_once(value, "splat", w.expr);
        }
        if (!is_valid(splat.first))
        {
            result.push_back(fail());
            continue;
        }
        auto const object = w.splat_member == 0 ? splat.first : again(splat.later, w.expr);
        auto const field = splat_letters.count > 0 ? i32(splat_letters.fields[w.splat_member]) : w.splat_member;
        result.push_back(
            add_expr(field_type(entry.at(object).type, field), w.expr, flat_member{.object = object, .member = field}));
    }
    written_closures = cc::move(handed);
    return result;
}

auto sgl::check::impl::flattener::closure_of(ast::expr_id expr) -> i32
{
    if (ast().at(expr).node.is<ast::lambda>())
    {
        closures.push_back({.file = file(),
                            .lambda = expr,
                            .bound = current()->bound,
                            .chain = current()->chain,
                            .owner = current()->function,
                            .bindings = current()->bindings,
                            .instance = current()->instance});
        return i32(closures.size() - 1);
    }
    auto const where = tables().target_at(expr);
    if (where.kind == target_kind::overload)
    {
        closures.push_back({.function = where.symbol});
        return i32(closures.size() - 1);
    }
    for (auto const& b : current()->bound)
        if (b.where == where && b.closure >= 0)
            return b.closure;
    mark_failed();
    return -1;
}

auto sgl::check::impl::flattener::flatten_closure_call(ast::expr_id id, type_id type, i32 handed, call_record const& record)
    -> flat_expr_id
{
    auto const values = flatten_written(c.out.at(record.written));
    auto const argument_closures = written_closures;
    auto const slots = c.out.at(record.slots);
    return call_closure(id, type, handed, values, slots, argument_closures);
}

auto sgl::check::impl::flattener::target_case_of(symbol_id constant) const -> i32
{
    auto const& s = c.out.at(constant);
    if (s.kind != symbol_kind::constant || !c.is_prelude_file(s.file))
        return -1;
    if (s.name == "target_api")
        return i32(compiled_for().api);
    if (s.name == "target_language")
        return i32(compiled_for().language);
    return -1;
}

auto sgl::check::impl::flattener::is_has_call(symbol_id callee) const -> bool
{
    // a call of a parameter of function type has no callee of its own
    if (!is_valid(callee))
        return false;
    // `ray_flags.has` is a prelude function of that name too, and `has(f: feature)` the one free of it
    auto const& s = c.out.at(callee);
    if (s.kind != symbol_kind::function || s.name != "has" || !c.is_prelude_file(s.file) || s.info < 0)
        return false;
    auto const parameters = c.out.at(c.out.functions[s.info].parameters);
    return parameters.size() == 1 && c.out.name_of(parameters[0].type) == "feature";
}

auto sgl::check::impl::flattener::refuses_target_in_const(cc::string_view what) -> bool
{
    if (!is_const_evaluation)
        return false;
    if (target_in_const.empty())
        target_in_const = cc::format("reads {}, which each compile stands for once a body is inlined, while a const is "
                                     "computed once for every compile; a function that returns it is inlined where "
                                     "it is called",
                                     what);
    mark_failed();
    return true;
}

auto sgl::check::impl::flattener::feature_case_of(ast::expr_id argument) -> i32
{
    if (!ast::is_valid(argument))
        return -1;
    auto const& where = tables().target_at(argument);
    if (where.kind == target_kind::enum_case)
        return where.index;
    // a parameter bound to a literal case, as an inlined `has(f)` of a function's own `f` is
    for (auto const& b : current()->bound)
        if (b.where == where && is_valid(b.literal))
            if (auto const* const e = entry.at(b.literal).node.try_as<flat_enum_value>())
                return e->case_index;
    if (where.kind == target_kind::symbol && c.out.at(where.symbol).kind == symbol_kind::constant
        && c.out.at(where.symbol).state == symbol_state::checked)
        if (auto const& info = c.out.constants[c.out.at(where.symbol).info]; info.kind == constant_kind::enum_case)
            return info.case_index;
    return -1;
}

auto sgl::check::impl::flattener::flatten_has(ast::expr_id id, type_id type, call_record const& record) -> flat_expr_id
{
    if (refuses_target_in_const("has"))
        return flat_expr_id::none;
    auto const written = c.out.at(record.written);
    auto const index = written.size() == 1 ? feature_case_of(written[0].expr) : -1;
    if (index < 0 || index >= k_feature_count)
    {
        unroll_reports.push_back({.file = file(),
                                  .where = c.span_of(file(), id),
                                  .detail = "has asks for a feature its call names as a constant once inlined, such "
                                            "as `has(.subgroups)`",
                                  .site = innermost_call()});
        mark_failed();
        meets_error = true;
        return flat_expr_id::none;
    }
    // EMIT-264: what the entry consults is a variant axis, whichever way this compile decides it
    entry.consulted.set(feature(index));
    auto const literal
        = add_expr(type, id, flat_bool_literal{.value = compiled_for().device_features.has(feature(index))});
    has_literals.push_back(literal);
    return literal;
}

auto sgl::check::impl::flattener::grants_of(ast::expr_id condition) -> feature_set
{
    if (!ast::is_valid(condition))
        return {};
    auto const* const call = ast().at(condition).node.try_as<ast::call>();
    if (call == nullptr)
        return {};
    auto const arguments = ast().at(call->arguments);
    if (call->is_short_circuit && arguments.size() == 2 && sgl::is_valid(call->op)
        && c.text_of(file(), c.file_of(file()).at(call->op).where) == "and")
        return grants_of(arguments[0].value) | grants_of(arguments[1].value);
    auto const record = tables().call_at(condition);
    if (record < 0 || !is_has_call(c.out.call_records[record].callee))
        return {};
    auto const written = c.out.at(c.out.call_records[record].written);
    auto const index = written.size() == 1 ? feature_case_of(written[0].expr) : -1;
    if (index < 0 || index >= k_feature_count || !compiled_for().device_features.has(feature(index)))
        return {};
    auto result = feature_set();
    result.set(feature(index));
    return result;
}

auto sgl::check::impl::flattener::is_by_target(symbol_id callee) const -> bool
{
    auto const& s = c.out.at(callee);
    return s.name == "by_target" && c.is_prelude_file(s.file);
}

auto sgl::check::impl::flattener::flatten_by_target(ast::expr_id id, type_id type, call_record const& record)
    -> flat_expr_id
{
    (void)flatten_written(c.out.at(record.written));
    auto const handed = written_closures;
    if (handed.size() != 2 || handed[0] < 0 || handed[1] < 0)
        return fail();
    auto const native = closure_block(id, type, handed[0], "native");
    auto const emulated = closure_block(id, type, handed[1], "emulated");
    if (!is_valid(native) || !is_valid(emulated))
        return fail();
    return add_expr(type, id, flat_by_target{.native = native, .emulated = emulated});
}

auto sgl::check::impl::flattener::closure_block(ast::expr_id id, type_id type, i32 handed, cc::string_view name)
    -> flat_expr_id
{
    auto const label = add_label(name);
    auto const outer = cc::move(block);
    block = {};
    auto const value = call_closure(id, type, handed, {}, {}, {});
    if (is_valid(value))
        add_stmt({.file = file(), .expr = id}, flat_leave{.target = label, .value = value});
    auto const body = add_list(block);
    block = cc::move(outer);
    if (!is_valid(value))
        return fail();
    return add_expr(type, id, flat_block{.label = label, .body = body});
}

auto sgl::check::impl::flattener::call_closure(ast::expr_id id,
                                               type_id type,
                                               i32 handed,
                                               cc::span<flat_expr_id const> values,
                                               cc::span<i32 const> slots,
                                               cc::span<i32 const> argument_closures) -> flat_expr_id
{
    // by value: flattening below may push closures, and the vector moves
    auto const f = closures[handed];
    // CHK-404: a function handed a candidate or a box is a decision the traversal calls in its loop
    auto const outer_decision = inlined_decision;
    auto decision = outer_decision;
    if (!values.empty() && is_valid(values[0]))
        if (auto const d = decision_of(entry.at(values[0]).type); d != stage::none)
            decision = d;
    if (is_valid(f.function))
    {
        inlined_decision = decision;
        auto const inlined = inline_bound(id, f.function, values, slots, argument_closures);
        inlined_decision = outer_decision;
        return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
    }
    auto const& lambda_ast = c.ast_of(f.file);
    auto const& l = lambda_ast.at(f.lambda).node.as<ast::lambda>();
    auto const fields = lambda_ast.at(l.parameters);
    auto bound = f.bound;
    auto const lambda_type = table_view{.module = &c.out, .file = f.file, .instance = f.instance}.type_at(f.lambda);
    auto const parameter_types = c.out.at(c.out.at(lambda_type).members);
    for (auto p = isize(0); p < fields.size() && p < slots.size(); ++p)
    {
        auto const k = slots[p];
        auto const field_index = i32(&fields[p] - lambda_ast.fields.data());
        auto const where = target{.kind = target_kind::parameter, .index = field_index};
        if (k < 0 || k >= values.size())
            return fail();
        if (c.out.at(parameter_types[p].type).kind == type_kind::function)
        {
            bound.push_back({.where = where, .closure = argument_closures[k]});
            continue;
        }
        auto const argument = values[k];
        if (!is_valid(argument))
            return fail();
        auto const* const ref = entry.at(argument).node.try_as<flat_local_ref>();
        if (ref != nullptr && is_substitutable(argument))
            bound.push_back({.where = where, .local = ref->local});
        else if (is_substitutable(argument))
            bound.push_back({.where = where, .literal = argument});
        else
        {
            auto const local = add_local(local_kind::let, c.text_of(f.file, fields[p].name), entry.at(argument).type);
            add_stmt(entry.at(argument).from, flat_let{.local = local, .value = argument});
            bound.push_back({.where = where, .local = local});
        }
    }
    raise_float_level(f.owner);
    frames.push_back({.function = f.owner,
                      .file = f.file,
                      .result = type,
                      .chain = f.chain,
                      .bound = cc::move(bound),
                      .bindings = f.bindings,
                      .instance = f.instance});
    inlined_decision = decision;
    auto const value = flatten_expr(l.body.value);
    inlined_decision = outer_decision;
    frames.remove_back();
    return value;
}

auto sgl::check::impl::flattener::decision_of(type_id type) const -> stage
{
    if (!is_valid(type) || type == checked_module::error_type || c.out.at(type).kind != type_kind::structure)
        return stage::none;
    auto const& s = c.out.at(c.out.at(type).symbol);
    if (!c.is_prelude_file(s.file))
        return stage::none;
    if (s.name == "triangle_candidate" || s.name == "procedural_candidate")
        return stage::any_hit;
    return s.name == "procedural_box" ? stage::intersection : stage::none;
}

auto sgl::check::impl::flattener::flatten_chain(ast::expr_id id, type_id type, ast::comparison_chain const& chain)
    -> flat_expr_id
{
    // copies: the AST is stable, and the spans are not needed past the loop
    auto const operands = ast().at(chain.operands);
    auto const operators = ast().at(chain.operators);
    if (operands.size() < 2 || operators.size() + 1 != operands.size())
        return fail();

    auto comparisons = cc::vector<flat_expr_id>();
    auto left = evaluated_once{.first = flatten_expr(operands[0])};
    for (auto i = isize(0); i < operators.size(); ++i)
    {
        auto const is_last = i + 1 == operators.size();
        auto const value = flatten_expr(operands[i + 1]);
        auto const right = is_last ? evaluated_once{.first = value} : evaluate_once(value, "chained", operands[i + 1]);
        if (!is_valid(left.first) || !is_valid(right.first))
            return fail();

        type_id const types[] = {entry.at(left.first).type, entry.at(right.first).type};
        auto const spelling = c.text_of(file(), c.file_of(file()).at(operators[i]).where);
        auto const callee = c.find_operator(file(), spelling, types);
        if (!is_valid(callee))
            return fail();
        flat_expr_id const arguments[] = {left.first, right.first};
        comparisons.push_back(builtin_call(id, callee, arguments));
        if (!is_last)
            left = {.first = again(right.later, operands[i + 1])};
    }

    // right-nested, so each comparison runs only when every one before it held
    auto result = comparisons.back();
    for (auto i = comparisons.size() - 2; i >= 0; --i)
        result = add_expr(type, id, flat_and{.lhs = comparisons[i], .rhs = result});
    return result;
}

auto sgl::check::impl::flattener::flatten_value_loop(ast::expr_id id, type_id type, ast::loop_expr const& loop)
    -> flat_expr_id
{
    auto const value_block = add_label("loop_value");
    auto const label = add_label("loop");
    current()->loops.push_back({.loop = label, .value_block = value_block});
    auto const body = flatten_body(loop.body);
    current()->loops.remove_back();

    auto const where = origin{.file = file(), .expr = id};
    flat_stmt_id const statements[] = {make_stmt(where, flat_loop{.label = label, .body = body})};
    return add_expr(type, id, flat_block{.label = value_block, .body = add_list(statements)});
}

auto sgl::check::impl::flattener::flatten_case_parts(ast::case_expr const& node, label_id value_block) -> flat_case
{
    auto const scrutinee = flatten_expr(node.value);
    auto result = flat_case{.scrutinee = scrutinee, .equality = equality_for(scrutinee)};
    if (is_valid(result.equality))
        result.equality_intrinsic = c.out.at(result.equality).intrinsic;
    auto arms = cc::vector<flat_arm>();
    auto has_default = false;

    for (auto const& arm : ast().at(node.arms))
    {
        auto const is_wildcard = ast::is_valid(arm.pattern) && ast().at(arm.pattern).node.is<ast::wildcard>();
        auto patterns = cc::vector<flat_expr_id>();
        if (!is_wildcard)
            collect_patterns(arm.pattern, patterns);
        auto const body = flatten_arm_body(arm.result, value_block);
        if (is_wildcard)
        {
            result.default_body = body;
            has_default = true;
        }
        else
            arms.push_back({.patterns = add_list(patterns), .body = body});
    }

    // Every tree has a default: without a `_` the source was exhaustive by its cases, and the last arm is it.
    if (!has_default && !arms.empty())
    {
        result.default_body = arms.back().body;
        arms.remove_back();
    }
    result.arms = add_arms(arms);
    return result;
}

auto sgl::check::impl::flattener::equality_for(flat_expr_id scrutinee) const -> symbol_id
{
    if (!is_valid(scrutinee))
        return symbol_id::none;
    auto const type = entry.at(scrutinee).type;
    if (!is_valid(type))
        return symbol_id::none;
    // An enum compares as the `int` its cases are (EVAL-64).
    auto const compared = c.out.is_plain_enum(type) ? int_type() : type;
    if (!is_valid(compared))
        return symbol_id::none;
    type_id const both[] = {compared, compared};
    return c.find_operator(file(), "==", both);
}

auto sgl::check::impl::flattener::bool_type() const -> type_id
{
    auto const* const found = c.prelude_names.get_ptr(builtins::k_bool);
    return found == nullptr || found->empty() ? type_id::none : c.out.at(found->front()).type;
}
