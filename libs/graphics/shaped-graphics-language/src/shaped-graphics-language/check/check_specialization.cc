#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Comparing types, and what an instance decides of a `const if` before its branches are checked (CHK-429, CHK-430).

namespace
{
constexpr auto error_type = checked_module::error_type;
} // namespace

type_id checker::type_parameter_stands_for(cc::string_view name) const
{
    for (auto i = type_parameter_names.size(); i > 0; --i)
        if (type_parameter_names[i - 1].first == name)
            return type_parameter_names[i - 1].second;
    return type_id::none;
}

bool checker::is_type_operand(function_scope const& scope, ast::expr_id expr) const
{
    auto const file = scope.file;
    auto const& node = ast_of(file).at(expr).node;
    // `scalar_of[T]` and a generic struct's instance, `ring[float, 4]`, are named by their head
    auto head = expr;
    if (auto const* const index = node.try_as<ast::index>())
        head = index->object;
    auto const* const n = ast::is_valid(head) ? ast_of(file).at(head).node.try_as<ast::name>() : nullptr;
    if (n != nullptr)
    {
        auto const text = text_of(file, n->where);
        if (scope.find_local(text) != nullptr)
            return false;
        if (head == expr && is_type_parameter_name(text))
        {
            // a value parameter is a constant in an instance, and a parameter with a value type in the open body
            auto const& t = out.at(type_parameter_stands_for(text));
            return t.kind != type_kind::constant && !(t.kind == type_kind::type_parameter && is_valid(t.element));
        }
        if (head != expr && text == "scalar_of")
            return true;
    }
    auto const* const found = symbols_named(file, head, &scope);
    if (found == nullptr || found->empty())
        return false;
    auto const kind = out.at(found->front()).kind;
    return kind == symbol_kind::structure || kind == symbol_kind::enumeration;
}

cc::optional<type_id> checker::check_type_comparison(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const arguments = ast.at(call.arguments);
    if (arguments.size() != 2)
        return cc::nullopt;
    for (auto const& a : arguments)
        if (!a.name.empty() || a.is_splat || !ast::is_valid(a.value))
            return cc::nullopt;
    auto const left = arguments[0].value;
    auto const right = arguments[1].value;
    if (!is_type_operand(scope, left) && !is_type_operand(scope, right))
        return cc::nullopt;

    // CHK-429: one operand naming a type makes both type positions
    auto const bool_type = type_of_builtin(builtins::k_bool, file, span_of(file, id));
    type_id types[2];
    for (auto k = 0; k < 2; ++k)
    {
        auto const operand = k == 0 ? left : right;
        if (!is_type_operand(scope, operand))
        {
            report(diagnostic_kind::type_mismatch, file, span_of(file, operand),
                   "a type compares with a type: `T == float`, never with a value");
            return error_type;
        }
        types[k] = resolve_type(file, operand, &scope);
        if (types[k] == error_type)
            return error_type;
    }
    // the open body judges names alone, and an instance knows both types
    if (!is_open(types[0]) && !is_open(types[1]) && bool_type != error_type)
    {
        auto const is_equal = types[0] == types[1];
        auto const is_negated = text_of(file, file_of(file).at(call.op).where) == "!=";
        set_target(file, id,
                   {.kind = target_kind::generic_value,
                    .index = i32(index_of(constant_type(bool_type, is_equal != is_negated ? 1u : 0u)))});
    }
    return bool_type;
}

cc::optional<bool> checker::known_condition(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return cc::nullopt;
    auto const& ast = ast_of(file);
    auto const& node = ast.at(expr).node;
    auto const& where = tables_of(file).target_at(expr);

    // a generic's own bool, and a type comparison an instance decided
    if (where.kind == target_kind::generic_value)
    {
        auto const& constant = out.types[where.index];
        if (out.name_of(constant.element) == "bool")
            return constant.count != 0;
        return cc::nullopt;
    }
    // `true`, `false`, and a const of either that no option reaches
    if (where.kind == target_kind::symbol && is_valid(where.symbol) && out.at(where.symbol).kind == symbol_kind::constant
        && out.at(where.symbol).state == symbol_state::checked && !is_target_constant(where.symbol))
    {
        auto const& c = out.constants[out.at(where.symbol).info];
        if (c.kind != constant_kind::enum_case || is_valid(c.option) || !c.computed_options.empty()
            || out.name_of(c.type) != "bool")
            return cc::nullopt;
        // bool's cases are `false`, then `true`
        return c.case_index == 1;
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr || !sgl::is_valid(call->op))
        return cc::nullopt;
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast.at(call->arguments);
    if (op == "not" && arguments.size() == 1)
    {
        auto const inner = known_condition(file, arguments[0].value);
        return inner.has_value() ? cc::optional<bool>(!inner.value()) : cc::nullopt;
    }
    if (arguments.size() != 2)
        return cc::nullopt;
    // three-valued: a known false decides an `and`, and a known true an `or`, whatever the other side is
    if (op == "and" || op == "or")
    {
        auto const is_and = op == "and";
        auto const a = known_condition(file, arguments[0].value);
        auto const b = known_condition(file, arguments[1].value);
        if ((a.has_value() && a.value() != is_and) || (b.has_value() && b.value() != is_and))
            return !is_and;
        if (a.has_value() && b.has_value())
            return is_and;
        return cc::nullopt;
    }
    // a comparison of numbers an instance knows: a value parameter, a literal, a plain const, an enum's case
    if (!call->is_short_circuit && call->spelling == ast::call_spelling::infix)
    {
        auto const x = known_number_of(file, arguments[0].value);
        auto const y = known_number_of(file, arguments[1].value);
        if (!x.has_value() || !y.has_value())
            return cc::nullopt;
        auto const a = x.value();
        auto const b = y.value();
        if (op == "==")
            return a == b;
        if (op == "!=")
            return a != b;
        if (op == "<")
            return a < b;
        if (op == "<=")
            return a <= b;
        if (op == ">")
            return a > b;
        if (op == ">=")
            return a >= b;
    }
    return cc::nullopt;
}

cc::optional<f64> checker::known_number_of(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return cc::nullopt;
    if (auto const n = number_of(file, expr); n.is_number)
        return n.is_integer ? f64(n.integer) : n.real;
    auto const& where = tables_of(file).target_at(expr);
    if (where.kind == target_kind::enum_case)
        return f64(where.index);
    if (where.kind == target_kind::generic_value)
    {
        auto const& constant = out.types[where.index];
        auto const element = out.name_of(constant.element);
        auto const bits = u32(constant.count);
        if (out.at(constant.element).kind == type_kind::enumeration || element == "int")
            return f64(i32(bits));
        if (element == "uint")
            return f64(bits);
        if (element == "float")
            return f64(cc::bit_cast<f32>(bits));
        return cc::nullopt;
    }
    if (where.kind == target_kind::symbol && is_valid(where.symbol) && out.at(where.symbol).kind == symbol_kind::constant
        && out.at(where.symbol).state == symbol_state::checked && !is_target_constant(where.symbol))
    {
        auto const& c = out.constants[out.at(where.symbol).info];
        if (is_valid(c.option) || !c.computed_options.empty())
            return cc::nullopt;
        switch (c.kind)
        {
        case constant_kind::integer:
            return c.is_unsigned ? f64(u32(c.integer)) : f64(c.integer);
        case constant_kind::enum_case:
            return f64(c.case_index);
        default:
            return cc::nullopt;
        }
    }
    return cc::nullopt;
}
