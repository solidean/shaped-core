#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// `case` and its arms: an arm matches when `scrutinee == pattern`, and the first match runs (CHK-153 to CHK-170).

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;

/// True for an expression that leaves its arm rather than giving it a value: the jumps of AST-40.
bool is_jump(ast::file_ast const& ast, ast::expr_id id)
{
    if (!ast::is_valid(id))
        return false;
    auto const& e = ast.at(id);
    return e.node.is<ast::return_expr>() || e.node.is<ast::break_expr>() || e.node.is<ast::continue_expr>();
}
} // namespace

void checker::check_pattern(function_scope& scope,
                            ast::expr_id pattern,
                            type_id scrutinee,
                            cc::vector<i32>& named_cases,
                            bool& is_all_constant)
{
    auto const file = scope.file;
    if (!ast::is_valid(pattern))
    {
        is_all_constant = false;
        return;
    }
    auto const& ast = ast_of(file);
    auto const& e = ast.at(pattern);
    auto const where = span_of(file, pattern);

    // `a or b` in a pattern is a list of patterns, not the `or` of CHK-116: its operands are no bools (CHK-157).
    if (auto const* const call = e.node.try_as<ast::call>(); call != nullptr && call->is_short_circuit
                                                             && sgl::is_valid(call->op)
                                                             && text_of(file, file_of(file).at(call->op).where) == "or")
    {
        for (auto const& argument : ast.at(call->arguments))
            check_pattern(scope, argument.value, scrutinee, named_cases, is_all_constant);
        set_type(file, pattern, scrutinee);
        return;
    }

    // `.point`, resolved against the scrutinee's type, which is the one place a leading dot stands today (CHK-152).
    if (auto const* const dot = e.node.try_as<ast::leading_dot>())
    {
        if (scrutinee == error_type)
            return;
        if (out.at(scrutinee).kind != type_kind::enumeration)
        {
            report(diagnostic_kind::unknown_member, file, where,
                   cc::format("{} is no enum, so it has no case to name with a leading dot", out.name_of(scrutinee)));
            is_all_constant = false;
            return;
        }
        auto const name = text_of(file, dot->name);
        auto const cases = out.at(out.at(scrutinee).cases);
        auto index = isize(-1);
        for (auto i = isize(0); i < cases.size(); ++i)
            if (cases[i].name == name)
                index = i;
        if (index < 0)
        {
            report(diagnostic_kind::unknown_member, file, where,
                   cc::format("the enum {} has no case {}", out.name_of(scrutinee), name));
            is_all_constant = false;
            return;
        }
        set_target(file, pattern,
                   {.kind = target_kind::enum_case, .symbol = out.at(scrutinee).symbol, .index = i32(index)});
        set_type(file, pattern, scrutinee);
        named_cases.push_back(i32(index));
        return;
    }

    auto const type = check_expr(scope, pattern);
    if (type == error_type || scrutinee == error_type)
    {
        is_all_constant = false;
        return;
    }
    if (type != scrutinee)
    {
        report(diagnostic_kind::type_mismatch, file, where,
               cc::format("a pattern of {} stands against a {}", out.name_of(scrutinee), out.name_of(type)));
        is_all_constant = false;
        return;
    }
    // `light_kind.point` written out is the same constant the leading dot is, and it counts for exhaustiveness too.
    auto const& target = out.files[file].target_at(pattern);
    if (target.kind == target_kind::enum_case)
        named_cases.push_back(target.index);
    // CHK-221: a const whose value is a case names that case, which is what makes `true` and `false` exhaustive.
    else if (target.kind == target_kind::symbol && out.at(target.symbol).kind == symbol_kind::constant
             && out.constants[out.at(target.symbol).info].kind == constant_kind::enum_case)
        named_cases.push_back(out.constants[out.at(target.symbol).info].case_index);
    else
        is_all_constant = false;
}

void checker::check_yield(function_scope& scope, source_span where, ast::expr_id value)
{
    auto const file = scope.file;
    // a `yield` with no value block around it was reported by the AST pass
    if (scope.value_blocks.empty())
    {
        (void)check_expr(scope, value);
        return;
    }
    // by index: checking the value may open a value block of its own, and the vector then moves
    auto const innermost = scope.value_blocks.size() - 1;
    scope.value_blocks[innermost].has_yield = true;

    if (!ast::is_valid(value))
    {
        report(diagnostic_kind::type_mismatch, file, where, "a yield carries the value of its block");
        return;
    }
    // CHK-82: a property's `-> T` is expected of each yield, which a literal converts to
    if (scope.value_blocks[innermost].is_expected)
    {
        (void)check_expected(scope, value, scope.value_blocks[innermost].value);
        return;
    }
    auto const type = check_expr(scope, value);
    if (type == error_type)
    {
        if (!is_valid(scope.value_blocks[innermost].value))
            scope.value_blocks[innermost].value = error_type;
        return;
    }
    auto const expected = scope.value_blocks[innermost].value;
    if (!is_valid(expected) || expected == error_type)
        scope.value_blocks[innermost].value = type;
    else if (type != expected)
        report(diagnostic_kind::type_mismatch, file, span_of(file, value),
               cc::format("this block yields {}, and this yield carries a {}", out.name_of(expected), out.name_of(type)));
}

type_id checker::check_case(function_scope& scope, ast::expr_id id, ast::case_expr const& node, bool yields_value, flow* ending)
{
    if (ending != nullptr)
        *ending = flow::falls_through;
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);

    auto const scrutinee = check_expr(scope, node.value);

    // CHK-155: an arm matches by `==`, so a scrutinee whose `==` does not resolve has no arm that could.
    if (scrutinee != error_type && out.at(scrutinee).kind != type_kind::enumeration)
    {
        type_id const both[] = {scrutinee, scrutinee};
        if (!is_valid(find_operator("==", both)))
        {
            report(diagnostic_kind::no_matching_overload, file, span_of(file, node.value),
                   cc::format("== is not declared for {}, so no arm can match it", out.name_of(scrutinee)));
            return error_type;
        }
    }

    auto named_cases = cc::vector<i32>();
    auto is_all_constant = true;
    auto has_wildcard = false;
    auto result = yields_value ? type_id::none : void_type;
    auto is_failed = false;
    auto reported_unreachable = false;
    // CHK-123: whether every arm leaves the list the `case` stands in
    auto every_arm_exits = true;
    auto arm_count = 0;

    for (auto const& arm : ast.at(node.arms))
    {
        // A line of the block that is no arm was reported by the AST pass, and keeps no pattern.
        if (!ast::is_valid(arm.pattern) && arm.result.kind == ast::body_kind::none)
        {
            is_failed = true;
            every_arm_exits = false;
            continue;
        }
        ++arm_count;

        if (has_wildcard && !reported_unreachable)
        {
            reported_unreachable = true;
            report(diagnostic_kind::unreachable_code, file, span_of(file, arm.form),
                   "an arm behind `_`, which matches everything");
        }

        if (ast::is_valid(arm.pattern) && ast.at(arm.pattern).node.is<ast::wildcard>())
        {
            has_wildcard = true;
            set_type(file, arm.pattern, scrutinee);
        }
        else
            check_pattern(scope, arm.pattern, scrutinee, named_cases, is_all_constant);

        // The arm's body: a value block of its own, so a `yield` inside it names this arm (AST-107).
        auto const visible = scope.locals.size();
        ++scope.depth;
        if (yields_value)
            scope.value_blocks.push_back({});

        auto arm_type = type_id::none;
        auto arm_exits = false;
        if (arm.result.kind == ast::body_kind::arrow && ast::is_valid(arm.result.value))
        {
            if (is_jump(ast, arm.result.value))
            {
                arm_exits = true;
                auto const& jump = ast.at(arm.result.value);
                auto const jump_where = span_of(file, arm.result.value);
                if (auto const* const r = jump.node.try_as<ast::return_expr>())
                    check_return(scope, jump_where, r->value);
                else if (auto const* const b = jump.node.try_as<ast::break_expr>())
                    check_break(scope, jump_where, b->value);
                set_type(file, arm.result.value, void_type);
            }
            else
                arm_type = check_expr(scope, arm.result.value);
        }
        else
        {
            auto const ends = check_statements(scope, arm.result.statements);
            // A `yield` ends its list too, so an arm counts as exiting only where it yielded nothing.
            arm_exits = ends == flow::exits;
            if (ends == flow::unknown)
                is_failed = true;
        }

        if (yields_value)
        {
            auto const block = scope.value_blocks.back();
            scope.value_blocks.remove_back();
            if (block.has_yield)
            {
                arm_exits = false;
                if (!is_valid(arm_type))
                    arm_type = block.value;
            }
        }
        --scope.depth;
        scope.locals.resize_down_to(visible);
        every_arm_exits = every_arm_exits && arm_exits;

        if (!yields_value || arm_exits)
            continue;
        if (!is_valid(arm_type))
        {
            report(diagnostic_kind::missing_value_in_arm, file, span_of(file, arm.form),
                   "this case is a value, so every arm gives one or leaves");
            is_failed = true;
            continue;
        }
        if (arm_type == error_type)
        {
            is_failed = true;
            continue;
        }
        if (!is_valid(result) || result == error_type)
            result = arm_type;
        else if (arm_type != result)
            report(diagnostic_kind::type_mismatch, file, span_of(file, arm.form),
                   cc::format("this case is {}, and this arm gives a {}", out.name_of(result), out.name_of(arm_type)));
    }

    // CHK-159: a `_` makes it exhaustive, and so does an enum whose every case a constant pattern named.
    auto is_exhaustive = has_wildcard;
    if (!has_wildcard && scrutinee != error_type)
    {
        auto missing = cc::string();
        if (out.at(scrutinee).kind == type_kind::enumeration && is_all_constant)
        {
            auto const cases = out.at(out.at(scrutinee).cases);
            for (auto i = isize(0); i < cases.size(); ++i)
            {
                auto is_named = false;
                for (auto const named : named_cases)
                    is_named = is_named || named == i;
                if (is_named)
                    continue;
                if (!missing.empty())
                    missing += ", ";
                missing.appendf(".{}", cases[i].name);
            }
            if (!missing.empty())
                report(diagnostic_kind::non_exhaustive_case, file, where, cc::move(missing));
            is_exhaustive = missing.empty();
        }
        else
            report(diagnostic_kind::non_exhaustive_case, file, where,
                   "this case needs a `_` arm, since nothing says its patterns cover every value");
    }

    // CHK-161: two arms that name one case, which is a mistake rather than a second chance.
    for (auto i = isize(0); i < named_cases.size(); ++i)
        for (auto j = isize(0); j < i; ++j)
            if (named_cases[i] == named_cases[j])
            {
                auto const cases = out.at(out.at(scrutinee).cases);
                report(diagnostic_kind::duplicate_case_pattern, file, where,
                       cc::format(".{}", cases[named_cases[i]].name));
                i = named_cases.size();
                break;
            }

    if (ending != nullptr && is_exhaustive && every_arm_exits && arm_count > 0)
        *ending = flow::exits;
    if (is_failed)
        return error_type;
    if (!yields_value)
        return void_type;
    return is_valid(result) ? result : error_type;
}
