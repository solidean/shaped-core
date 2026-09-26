#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;

/// What a list that holds `a` and then `b` ends in, where `b` only runs when `a` falls through.
flow then(flow a, flow b)
{
    return a == flow::falls_through ? b : a;
}

/// What two alternative lists end in together.
flow either(flow a, flow b)
{
    if (a == flow::falls_through || b == flow::falls_through)
        return flow::falls_through;
    return a == flow::unknown || b == flow::unknown ? flow::unknown : flow::exits;
}
} // namespace

// ---- lists and scopes -----------------------------------------------------------------------------------------------

flow checker::check_statements(function_scope& scope, ast::range_of<ast::stmt_id> statements)
{
    auto result = flow::falls_through;
    auto is_reported = false;
    auto const& ast = ast_of(scope.file);
    for (auto const s : ast.at(statements))
    {
        // A test never runs where it stands, so it is no code a jump in front of it makes unreachable.
        auto const* const d = ast.at(s).node.try_as<ast::decl_stmt>();
        auto const is_test
            = d != nullptr && ast::is_valid(d->declaration) && ast.at(d->declaration).node.is<ast::test_decl>();
        if (result == flow::exits && !is_reported && !is_test)
        {
            report(diagnostic_kind::unreachable_code, scope.file, span_of(scope.file, s), "");
            is_reported = true;
        }
        result = then(result, check_stmt(scope, s));
    }
    return result;
}

flow checker::check_nested(function_scope& scope, ast::body const& body)
{
    auto const visible = scope.locals.size();
    ++scope.depth;
    auto const result = check_statements(scope, body.statements);
    --scope.depth;
    scope.locals.resize_down_to(visible);
    return result;
}

void checker::declare_local(function_scope& scope, local_name local)
{
    // CHK-53: a later local shadows every earlier local and parameter of its name, and lookup finds the newest.
    // It is a local of its own, so a later pass mints it a name of its own too.
    local.depth = scope.depth;
    scope.locals.push_back(local);
}

// ---- statements -----------------------------------------------------------------------------------------------------

flow checker::check_stmt(function_scope& scope, ast::stmt_id stmt)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const& s = ast.at(stmt);
    auto const where = span_of(file, stmt);
    judge_attributes(file, s.attributes, {}, "a statement");

    auto result = flow::falls_through;
    s.node.visit([&](ast::let_stmt const& let) { check_let(scope, stmt, let); }, [&](ast::assign_stmt const& assign)
                 { check_assign(scope, stmt, assign); }, [&](ast::if_stmt const& chain)
                 { result = check_if(scope, chain); }, [&](ast::for_stmt const& loop) { check_for(scope, stmt, loop); },
                 [&](ast::while_stmt const& loop)
                 {
                     check_condition(scope, loop.condition);
                     scope.loops.push_back({});
                     // A `while` ends when its condition says so, whatever the condition is, so what follows it is reachable.
                     (void)check_nested(scope, loop.body);
                     scope.loops.remove_back();
                 },
                 [&](ast::print_stmt const& print) { (void)check_expr(scope, print.message); },
                 [&](ast::expr_stmt const& e)
                 {
                     if (!ast::is_valid(e.value))
                         return;
                     auto const& value = ast.at(e.value);
                     if (auto const* const r = value.node.try_as<ast::return_expr>())
                     {
                         check_return(scope, where, r->value);
                         result = flow::exits;
                     }
                     else if (auto const* const b = value.node.try_as<ast::break_expr>())
                     {
                         check_break(scope, where, b->value);
                         result = flow::exits;
                     }
                     else if (value.node.is<ast::continue_expr>())
                         result = flow::exits;
                     else if (auto const* const y = value.node.try_as<ast::yield_expr>())
                     {
                         check_yield(scope, where, y->value);
                         result = flow::exits;
                     }
                     else if (auto const* const c = value.node.try_as<ast::case_expr>())
                         set_type(file, e.value, check_case(scope, e.value, *c, false, &result));
                     else if (auto const* const loop = value.node.try_as<ast::loop_expr>())
                     {
                         auto has_break = false;
                         set_type(file, e.value, check_loop(scope, e.value, *loop, false, has_break));
                         result = has_break ? flow::falls_through : flow::exits;
                     }
                     else
                     {
                         // A call is evaluated and its value dropped: it may have been written for its effect.
                         // Whether a PURE one is worth a statement is the AST pass's `no-effect` warning, and no business of this pass.
                         auto const type = check_expr(scope, e.value);
                         auto const* const call = value.node.try_as<ast::call>();
                         auto const is_application = call != nullptr
                                                  && (call->spelling == ast::call_spelling::paren
                                                      || call->spelling == ast::call_spelling::juxtaposition);
                         // CHK-225: in a test a line of type bool is a check; the AST pass left every other line to this one.
                         if (scope.is_test && type == type_of_builtin(builtins::k_bool, file, where))
                             return;
                         if (scope.is_test && !is_application && type != error_type && type != void_type)
                             report(diagnostic_kind::no_effect, file, where, "");
                         else if (type != error_type && type != void_type && call == nullptr)
                             unsupported(file, where, "an expression statement");
                     }
                 },
                 [&](ast::assert_stmt const& a)
                 {
                     // CHK-227: a bool condition; the run stops where it is false, and a target writes nothing of it
                     check_condition(scope, a.condition);
                     if (ast::is_valid(a.message))
                         unsupported(file, span_of(file, a.message), "an assert message");
                 },
                 [&](ast::decl_stmt const& d)
                 {
                     // CHK-224: a test in a function body is checked on its own, with the function's names visible and unusable
                     if (ast::is_valid(d.declaration) && ast.at(d.declaration).node.is<ast::test_decl>())
                     {
                         // one inside another test was reported by the AST pass
                         if (!scope.is_test)
                             add_test(file, d.declaration, cc::format("fun {}", out.at(scope.function).name), &scope);
                         return;
                     }
                     // CHK-237: a body's `require` stands among its own lines; a block of its own is not carried yet
                     if (auto const* const r = ast::is_valid(d.declaration)
                                                 ? ast.at(d.declaration).node.try_as<ast::require_decl>()
                                                 : nullptr)
                     {
                         judge_attributes(file, ast.at(d.declaration).attributes, {}, "a require");
                         if (scope.depth > 0)
                             unsupported(file, where, "a `require` inside a block");
                         else
                             read_require(file, *r, require_scope::body, scope.function);
                         return;
                     }
                     unsupported(file, where, "a declaration inside a function");
                 },
                 [&](ast::invalid_stmt const&) { result = flow::unknown; });
    return result;
}

void checker::check_let(function_scope& scope, ast::stmt_id id, ast::let_stmt const& let)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);

    auto type = error_type;
    if (ast::is_valid(let.value))
        type = check_expr(scope, let.value);
    else
        unsupported(file, where, "a let without a value");

    if (ast::is_valid(let.type))
    {
        auto const declared = resolve_value_type(file, let.type, &scope);
        if (type != error_type && declared != error_type && type != declared)
            report(diagnostic_kind::type_mismatch, file, span_of(file, let.value),
                   cc::format("expected {}, got {}", out.name_of(declared), out.name_of(type)));
        type = declared;
    }

    if (!ast::is_valid(let.pattern))
        return;
    auto const* const n = ast.at(let.pattern).node.try_as<ast::name>();
    if (n == nullptr)
    {
        if (!ast.at(let.pattern).node.is<ast::invalid_expr>())
            unsupported(file, span_of(file, let.pattern), "a pattern in let");
        return;
    }

    auto const self = target{.kind = target_kind::local, .index = i32(id)};
    set_type(file, let.pattern, type);
    set_target(file, let.pattern, self);
    judge_shadowing(file, text_of(file, n->where), n->where);
    // only now: the value of `let x = x` does not see the `x` it declares
    declare_local(scope, {.name = text_of(file, n->where), .where = self, .type = type, .is_mut = let.is_mut});
}

void checker::check_assign(function_scope& scope, ast::stmt_id id, ast::assign_stmt const& assign)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);

    auto const place = check_expr(scope, assign.target);
    auto const value = check_expr(scope, assign.value);

    // A buffer element is a place of its own: `work.dst[i] = v`, and only where the buffer is `mut`.
    auto const* const indexed = ast::is_valid(assign.target) ? ast.at(assign.target).node.try_as<ast::index>() : nullptr;
    if (indexed != nullptr)
    {
        auto const object = out.files[file].type_at(indexed->object);
        // What it is when it is no buffer at all was reported by `check_index`.
        if (object != type_id::none && out.at(object).kind == type_kind::buffer && !out.at(object).is_mut)
            report(diagnostic_kind::not_assignable, file, span_of(file, assign.target),
                   "this buffer is read-only; `mut buffer[T]` declares one a shader writes");
    }

    // The place is otherwise a mutable local, or a member of one at any depth.
    auto root = assign.target;
    while (ast::is_valid(root) && ast.at(root).node.is<ast::member>())
        root = ast.at(root).node.as<ast::member>().object;
    if (indexed == nullptr && place != error_type && ast::is_valid(root))
    {
        auto const* const n = ast.at(root).node.try_as<ast::name>();
        auto const& named = out.files[file].target_at(root);
        auto const* local = static_cast<local_name const*>(nullptr);
        for (auto const& l : scope.locals)
            if (n != nullptr && l.where == named)
                local = &l;
        auto const target_where = span_of(file, assign.target);
        if (n == nullptr || (local == nullptr && named.kind != target_kind::symbol))
            report(diagnostic_kind::not_assignable, file, target_where, "only a local, or a member of one, is assigned");
        else if (local == nullptr)
            report(diagnostic_kind::not_assignable, file, target_where,
                   cc::format("{} is a binding, which no shader writes", text_of(file, n->where)));
        else if (local->where.kind == target_kind::parameter)
            report(diagnostic_kind::not_assignable, file, target_where,
                   cc::format("{} is a parameter, which is a value", local->name));
        else if (!local->is_mut)
            report(diagnostic_kind::not_assignable, file, target_where,
                   cc::format("{} is immutable; `let mut` declares a local an assignment may name", local->name));
    }

    if (place == error_type || value == error_type || !sgl::is_valid(assign.op))
        return;

    auto const op = text_of(file, file_of(file).at(assign.op).where);
    if (op == "=")
    {
        if (place != value)
            report(diagnostic_kind::type_mismatch, file, span_of(file, assign.value),
                   cc::format("expected {}, got {}", out.name_of(place), out.name_of(value)));
        return;
    }

    // `x += v` is `x = x + v`: the operator resolves like any other, and its result has to fit the place again.
    auto const spelling = op.subview({.offset = 0, .size = op.size() - 1});
    type_id const types[] = {place, value};
    auto const chosen = resolve_operator(file, where, spelling, types);
    if (!is_valid(chosen))
        return;
    auto const result = out.functions[out.at(chosen).info].result;
    if (result != place)
        report(diagnostic_kind::type_mismatch, file, where,
               cc::format("{} {} {} is {}, and the place is {}", out.name_of(place), spelling, out.name_of(value),
                          out.name_of(result), out.name_of(place)));
}

void checker::check_condition(function_scope& scope, ast::expr_id condition)
{
    auto const type = check_expr(scope, condition);
    auto const expected = type == error_type ? error_type : type_of_builtin(builtins::k_bool, scope.file, {});
    if (type != error_type && expected != error_type && type != expected)
        report(diagnostic_kind::type_mismatch, scope.file, span_of(scope.file, condition),
               cc::format("a condition is a bool, got {}", out.name_of(type)));
}

flow checker::check_if(function_scope& scope, ast::if_stmt const& chain)
{
    auto const branches = ast_of(scope.file).at(chain.branches);
    auto result = flow::exits;
    auto has_else = false;
    for (auto const& branch : branches)
    {
        if (ast::is_valid(branch.condition))
            check_condition(scope, branch.condition);
        else
            has_else = true;
        result = either(result, check_nested(scope, branch.then));
    }
    // Without an `else` the chain can be skipped whole.
    return has_else ? result : flow::falls_through;
}

void checker::check_for(function_scope& scope, ast::stmt_id id, ast::for_stmt const& loop)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const int_type = type_of_builtin(builtins::k_int, file, span_of(file, id));

    auto const* const r = ast::is_valid(loop.iterable) ? ast.at(loop.iterable).node.try_as<ast::range>() : nullptr;
    if (r == nullptr)
    {
        if (check_expr(scope, loop.iterable) != error_type)
            unsupported(file, span_of(file, loop.iterable), "a for over anything but an int range");
    }
    else
    {
        auto const bound = [&](ast::expr_id value)
        {
            auto const type = check_expr(scope, value);
            if (type != error_type && int_type != error_type && type != int_type)
                report(diagnostic_kind::type_mismatch, file, span_of(file, value),
                       cc::format("a for runs over int, got {}", out.name_of(type)));
        };
        bound(r->first);
        bound(r->last);
        // `..=` has no flat form: `end + 1` wraps where the last int is the end
        if (sgl::is_valid(r->op) && text_of(file, file_of(file).at(r->op).where) != "..<")
            unsupported(file, span_of(file, loop.iterable), "a for over a range that is not `..<`");
    }

    if (ast::is_valid(loop.type))
    {
        auto const declared = resolve_type(file, loop.type, &scope);
        if (declared != error_type && int_type != error_type && declared != int_type)
            report(diagnostic_kind::type_mismatch, file, span_of(file, loop.type),
                   cc::format("a for runs over int, got {}", out.name_of(declared)));
    }

    auto const visible = scope.locals.size();
    ++scope.depth;
    auto const* const n = ast::is_valid(loop.variable) ? ast.at(loop.variable).node.try_as<ast::name>() : nullptr;
    if (n != nullptr)
    {
        auto const self = target{.kind = target_kind::local, .index = i32(id)};
        set_type(file, loop.variable, int_type);
        set_target(file, loop.variable, self);
        judge_shadowing(file, text_of(file, n->where), n->where);
        declare_local(scope, {.name = text_of(file, n->where), .where = self, .type = int_type});
    }
    scope.loops.push_back({});
    (void)check_statements(scope, loop.body.statements);
    scope.loops.remove_back();
    --scope.depth;
    scope.locals.resize_down_to(visible);
}

type_id checker::check_loop(function_scope& scope,
                            ast::expr_id id,
                            ast::loop_expr const& loop,
                            bool yields_value,
                            bool& has_break)
{
    scope.loops.push_back({.yields_value = yields_value});
    (void)check_nested(scope, loop.body);
    auto const done = scope.loops.back();
    scope.loops.remove_back();

    has_break = done.has_break;
    if (!yields_value)
        return void_type;
    if (!done.has_break)
    {
        unsupported(scope.file, span_of(scope.file, id), "a loop without a break as a value");
        return error_type;
    }
    return is_valid(done.value) ? done.value : error_type;
}

void checker::check_break(function_scope& scope, source_span where, ast::expr_id value)
{
    auto const file = scope.file;
    // a `break` without a loop was reported by the AST pass
    if (scope.loops.empty())
    {
        (void)check_expr(scope, value);
        return;
    }
    // by index: checking the value may open a loop of its own, and the vector then moves
    auto const innermost = scope.loops.size() - 1;
    scope.loops[innermost].has_break = true;
    auto const yields_value = scope.loops[innermost].yields_value;

    if (!ast::is_valid(value))
    {
        if (yields_value)
            report(diagnostic_kind::type_mismatch, file, where, "this loop is a value, so its break carries one");
        return;
    }
    auto const type = check_expr(scope, value);
    if (!yields_value)
    {
        if (type != error_type)
            report(diagnostic_kind::type_mismatch, file, span_of(file, value),
                   "nothing reads the value of this loop, so its break carries none");
        return;
    }
    if (type == error_type)
    {
        scope.loops[innermost].value = is_valid(scope.loops[innermost].value) ? scope.loops[innermost].value : error_type;
        return;
    }
    auto const expected = scope.loops[innermost].value;
    if (!is_valid(expected) || expected == error_type)
        scope.loops[innermost].value = type;
    else if (type != expected)
        report(diagnostic_kind::type_mismatch, file, span_of(file, value),
               cc::format("an earlier break of this loop carries {}, got {}", out.name_of(expected), out.name_of(type)));
}

void checker::check_return(function_scope& scope, source_span where, ast::expr_id value)
{
    auto const file = scope.file;
    auto const name = cc::string_view(out.at(scope.function).name);
    if (!ast::is_valid(value))
    {
        if (scope.result != error_type && scope.result != void_type)
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("{} returns {}, and this return has no value", name, out.name_of(scope.result)));
        return;
    }
    if (scope.result == void_type)
    {
        auto const type = check_expr(scope, value);
        if (type != error_type && type != void_type)
            report(diagnostic_kind::type_mismatch, file, span_of(file, value),
                   cc::format("{} returns void, got {}", name, out.name_of(type)));
        return;
    }
    if (ast_of(file).at(value).node.is<ast::object>())
    {
        convert_object(scope, value, scope.result);
        return;
    }

    auto const type = check_expr(scope, value);
    if (type != error_type && scope.result != error_type && type != scope.result)
        report(diagnostic_kind::type_mismatch, file, span_of(file, value),
               cc::format("expected {}, got {}", out.name_of(scope.result), out.name_of(type)));
}

// ---- recursion ------------------------------------------------------------------------------------------------------

void checker::find_recursion()
{
    // 0 is unvisited, 1 is on the path, 2 is done; one entry per function, as `notes`
    auto state = cc::vector<u8>::create_filled(notes.size(), 0);
    auto path = cc::vector<symbol_id>();

    auto const visit = [&](auto&& self, symbol_id f) -> void
    {
        state[out.at(f).info] = 1;
        path.push_back(f);
        for (auto i = isize(0); i < calls.size(); ++i)
        {
            // by value: a report does not touch `calls`, and a copy keeps this loop honest if one ever does
            auto const edge = calls[i];
            if (edge.caller != f)
                continue;
            auto const seen = state[out.at(edge.callee).info];
            if (seen == 0)
                self(self, edge.callee);
            if (seen != 1)
                continue;

            // The loop starts where the callee entered the path and closes on it again.
            auto loop = cc::string();
            auto is_inside = false;
            for (auto const s : path)
            {
                is_inside = is_inside || s == edge.callee;
                if (!is_inside)
                    continue;
                loop.appendf("{} -> ", out.at(s).name);
                notes[out.at(s).info].is_recursive = true;
            }
            loop += out.at(edge.callee).name;
            report(diagnostic_kind::recursive_call, edge.file, edge.where, cc::move(loop));
        }
        path.remove_back();
        state[out.at(f).info] = 2;
    };

    for (auto i = isize(0); i < out.symbols.size(); ++i)
    {
        auto const& s = out.symbols[i];
        if (s.kind == symbol_kind::function && s.info >= 0 && state[s.info] == 0)
            visit(visit, symbol_id(i));
    }
}

bool checker::inlines_whole(symbol_id function)
{
    auto const info = out.at(function).info;
    if (info < 0)
        return false;
    if (notes[info].inlines_whole != 0)
        return notes[info].inlines_whole == 1;

    auto result
        = out.at(function).state == symbol_state::checked && notes[info].is_body_sound && !notes[info].is_recursive;
    // Set before the callees are asked: a loop of calls is recursive, so nothing on it gets here twice.
    notes[info].inlines_whole = result ? 1 : 2;
    for (auto i = isize(0); result && i < calls.size(); ++i)
        if (calls[i].caller == function)
            result = inlines_whole(calls[i].callee);
    notes[info].inlines_whole = result ? 1 : 2;
    return result;
}
