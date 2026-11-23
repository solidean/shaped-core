#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/query.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Queries (the spec's checking file, CHK-540 to CHK-546): `let q = world.query(r)`, `for c in q.candidates:` and
// `q.hit`, which the flattener writes as a trace's two forms (check/flatten_query.cc).
// What a reader checks by eye is checked here, from the text: where a loop stands and when a hit is read.
// What only the inlined tree shows, a query handed on through a call, is the flattener's to judge.

namespace
{
constexpr auto error_type = checked_module::error_type;

cc::string_view geometry_name(check::type_info const& t)
{
    return k_geometry_kinds[t.format];
}

/// The `query_let` of `local`, a `let` of the body being checked; null for a parameter or another local.
query_let* query_let_of(function_scope& scope, local_name const* local)
{
    if (local == nullptr || local->where.kind != target_kind::local)
        return nullptr;
    for (auto& q : scope.query_lets)
        if (q.let == ast::stmt_id(local->where.index))
            return &q;
    return nullptr;
}

bool is_among(cc::span<ast::stmt_id const> statements, ast::stmt_id id)
{
    for (auto const s : statements)
        if (s == id)
            return true;
    return false;
}
} // namespace

type_id checker::query_helper_result(cc::string_view name, i32 file, source_span where)
{
    auto const* const found = prelude_names.get_ptr(name);
    if (found == nullptr || found->empty())
    {
        report(diagnostic_kind::unknown_name, file, where, cc::format("{}, which the prelude must declare", name));
        return error_type;
    }
    auto const helper = found->front();
    if (demand(helper, file, where) != symbol_state::checked || out.at(helper).info < 0)
        return error_type;
    return out.functions[out.at(helper).info].result;
}

bool checker::is_candidates(i32 file, ast::expr_id iterable) const
{
    if (!ast::is_valid(iterable))
        return false;
    auto const* const member = ast_of(file).at(iterable).node.try_as<ast::member>();
    return member != nullptr && text_of(file, member->name) == "candidates";
}

bool checker::judge_query_let(function_scope& scope, ast::stmt_id id, ast::let_stmt const& let, type_id type)
{
    auto const file = scope.file;
    if (type == error_type || !is_query_kind(out.at(type).kind) || is_prelude_file(file))
        return true;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);
    if (out.at(type).kind == type_kind::query_candidate)
    {
        report(diagnostic_kind::query_escapes, file, where,
               "a candidate is the variable of its loop, and no other local holds it; `c.triangle()` and `c.box()` "
               "give a value to keep");
        return false;
    }
    // CHK-541: a query is the `let` of the `world.query` that gave it, and nothing else holds it
    auto const* const call = ast::is_valid(let.value) ? ast.at(let.value).node.try_as<ast::call>() : nullptr;
    auto const& callee = ast::is_valid(let.value) ? tables_of(file).target_at(let.value) : target{};
    auto const is_start = call != nullptr && callee.kind == target_kind::overload && is_valid(callee.symbol)
                       && out.at(callee.symbol).name == "query" && is_prelude_file(out.at(callee.symbol).file);
    if (let.is_mut)
    {
        report(diagnostic_kind::query_escapes, file, where,
               "a `let mut` of a query, which nothing could assign: a query is the `let` of its `world.query`");
        return false;
    }
    if (!is_start)
    {
        report(diagnostic_kind::query_escapes, file, ast::is_valid(let.value) ? span_of(file, let.value) : where,
               "a query is the `let` of the `world.query` that gave it, and no other local holds it");
        return false;
    }
    scope.query_lets.push_back(
        {.let = id, .block = scope.blocks.empty() ? ast::range_of<ast::stmt_id>{} : scope.blocks.back()});
    return true;
}

type_id checker::check_query_member(function_scope& scope, ast::expr_id id, ast::member const& member, type_id object)
{
    auto const file = scope.file;
    auto const name = text_of(file, member.name);
    auto const& t = out.at(object);
    auto const where = span_of(file, id);
    if (t.kind == type_kind::trace_query)
    {
        if (name == "candidates")
        {
            report(diagnostic_kind::query_traversal_misplaced, file, where,
                   "a query's candidates stand behind the `in` of its candidate loop, `for c in q.candidates:`, and "
                   "nowhere else");
            return error_type;
        }
        if (name != "hit")
        {
            report(diagnostic_kind::unknown_member, file, member.name,
                   cc::format("{} has no member {}; a query has its `candidates`, stepped through by a loop, and its "
                              "`hit`",
                              out.name_of(object), name));
            return error_type;
        }
        // CHK-546: after the query's loop alone, which the text shows where the query is a `let` of this body
        auto const* const object_name = ast_of(file).at(member.object).node.try_as<ast::name>();
        auto const* const local = object_name != nullptr ? scope.find_local(text_of(file, object_name->where)) : nullptr;
        if (auto const* const q = query_let_of(scope, local); q != nullptr && !is_prelude_file(file))
        {
            if (!ast::is_valid(q->loop))
            {
                report(diagnostic_kind::query_hit_too_early, file, where,
                       "a query's hit is read after its candidate loop, and this query has none before here");
                return error_type;
            }
            if (q->is_loop_open)
            {
                report(diagnostic_kind::query_hit_too_early, file, where,
                       "a query's hit is read after its candidate loop, where the traversal has settled it; inside the "
                       "loop what is committed changes with every candidate, and `c.t_current` reads it");
                return error_type;
            }
        }
        auto const result = query_helper_result(cc::format("query_native_hit_{}", geometry_name(t)), file, where);
        set_target(file, id, {.kind = target_kind::query_member, .index = i32(query_member::hit)});
        return result;
    }

    // a candidate's member, read without a call
    for (auto i = isize(0); i < isize(sizeof(k_query_members) / sizeof(k_query_members[0])); ++i)
    {
        auto const& m = k_query_members[i];
        if (m.name != name || query_member(i) == query_member::hit || query_member(i) == query_member::commit_at)
            continue;
        if (m.is_call)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, member.name,
                   cc::format("a candidate's {0} is called, `c.{0}()`", name));
            return error_type;
        }
        if (!has_member_of(t.format, m.kind))
        {
            report(diagnostic_kind::unknown_member, file, member.name,
                   cc::format("{} has no member {}, which only a {} candidate has; this query's structure holds {}",
                              out.name_of(object), name, m.kind == query_member_kind::triangle ? "triangle" : "box",
                              geometry_name(t)));
            return error_type;
        }
        auto const result = query_helper_result(cc::format("query_native_{}", name), file, where);
        set_target(file, id, {.kind = target_kind::query_member, .index = i32(i)});
        return result;
    }
    report(diagnostic_kind::unknown_member, file, member.name,
           cc::format("{} has no member {}", out.name_of(object), name));
    return error_type;
}

type_id checker::check_query_call(function_scope& scope, ast::expr_id id, ast::call const& call, type_id receiver)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const& member = ast.at(call.callee).node.as<ast::member>();
    auto const name = text_of(file, member.name);
    auto const& t = out.at(receiver);
    auto const where = span_of(file, id);
    auto const arguments = ast.at(call.arguments);
    if (t.kind == type_kind::trace_query)
    {
        for (auto const& a : arguments)
            (void)check_expr(scope, a.value);
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no function {}; a query has its `candidates` and its `hit`", out.name_of(receiver),
                          name));
        return error_type;
    }
    auto const geometry = geometry_name(t);
    if (name == "commit")
    {
        // CHK-545: a triangle is accepted as it is, and a box's primitive is reported at a `t` of the body's
        auto const is_box = arguments.size() == 1;
        if (arguments.size() > 1 || (arguments.size() == 1 && !arguments[0].name.empty()))
        {
            for (auto const& a : arguments)
                (void)check_expr(scope, a.value);
            report(diagnostic_kind::no_matching_overload, file, where,
                   "a candidate's commit is `c.commit()` for a triangle, or `c.commit(t)` for a box");
            return error_type;
        }
        if (is_box)
        {
            auto const float_type = type_of_builtin(builtins::k_float, file, where);
            auto const given = check_expected(scope, arguments[0].value, float_type, "a box's commit");
            if (given == error_type)
                return error_type;
        }
        if (!has_member_of(t.format, is_box ? query_member_kind::box : query_member_kind::triangle))
        {
            report(diagnostic_kind::no_matching_overload, file, where,
                   is_box ? cc::format("`c.commit(t)` reports a box's primitive, and a query of {} meets no box; "
                                       "`c.commit()` accepts a triangle",
                                       geometry)
                          : cc::format("`c.commit()` accepts a triangle, and a query of {} meets no triangle; "
                                       "`c.commit(t)` reports a box's primitive at `t`",
                                       geometry));
            return error_type;
        }
        set_target(
            file, id,
            {.kind = target_kind::query_member, .index = i32(is_box ? query_member::commit_at : query_member::commit)});
        return type_of_builtin(builtins::k_bool, file, where);
    }
    if (name == "triangle" || name == "box")
    {
        for (auto const& a : arguments)
            (void)check_expr(scope, a.value);
        auto const is_triangle = name == "triangle";
        if (!arguments.empty())
        {
            report(diagnostic_kind::no_matching_overload, file, where, cc::format("`c.{}()` takes no argument", name));
            return error_type;
        }
        if (!has_member_of(t.format, is_triangle ? query_member_kind::triangle : query_member_kind::box))
        {
            report(diagnostic_kind::unknown_member, file, member.name,
                   cc::format("{} has no member {}, which only a {} candidate has; this query's structure holds {}",
                              out.name_of(receiver), name, is_triangle ? "triangle" : "box", geometry));
            return error_type;
        }
        auto const result = query_helper_result(cc::format("query_native_{}", name), file, where);
        set_target(
            file, id,
            {.kind = target_kind::query_member, .index = i32(is_triangle ? query_member::triangle : query_member::box)});
        return result;
    }
    for (auto const& a : arguments)
        (void)check_expr(scope, a.value);
    report(
        diagnostic_kind::unknown_member, file, member.name,
        cc::format("{} has no function {}; its calls are `commit`, `triangle` and `box`", out.name_of(receiver), name));
    return error_type;
}

void checker::check_candidate_loop(function_scope& scope, ast::stmt_id id, ast::for_stmt const& loop)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const& member = ast.at(loop.iterable).node.as<ast::member>();
    auto const object = check_expr(scope, member.object);
    auto const where = span_of(file, loop.iterable);

    auto candidate = error_type;
    if (object != error_type && out.at(object).kind != type_kind::trace_query)
    {
        unsupported(file, where, "a for over anything but an int range or a query's candidates");
    }
    else if (object != error_type)
    {
        set_type(file, loop.iterable, object);
        candidate = resource_type({.kind = type_kind::query_candidate, .format = out.at(object).format});
        // CHK-542: one loop, in the block of the query's `let`, after it and at its level
        auto const* const object_name = ast.at(member.object).node.try_as<ast::name>();
        auto const* const local = object_name != nullptr ? scope.find_local(text_of(file, object_name->where)) : nullptr;
        auto* const q = query_let_of(scope, local);
        auto const block = scope.blocks.empty() ? ast::range_of<ast::stmt_id>{} : scope.blocks.back();
        auto const statements = ast.at(block);
        if (q == nullptr)
            report(diagnostic_kind::query_traversal_misplaced, file, where,
                   "a candidate loop stands in the block of its query's `let`, and this query has none in this body; a "
                   "query a function is handed is stepped through where its `let` stands");
        else if (ast::is_valid(q->loop))
            report(diagnostic_kind::query_traversal_misplaced, file, where,
                   "a query is stepped through once, by one loop, and this query has one already");
        else if (q->block.first != block.first || q->block.count != block.count || !is_among(statements, id))
            report(diagnostic_kind::query_traversal_misplaced, file, where,
                   "a candidate loop stands in the block of its query's `let`, at its level, so nothing runs the loop "
                   "more often than the trace or skips it");
        else
            q->loop = id;
        if (q != nullptr && q->loop == id)
            q->is_loop_open = true;
    }
    // CHK-542: how often a candidate loop runs is the driver's, so nothing states it
    auto const& attributes = ast.at(id).attributes;
    for (auto const& a : ast.at(attributes))
        unsupported(file, a.name, "an attribute on a candidate loop, which runs as often as the driver decides");
    if (ast::is_valid(loop.type))
        unsupported(file, span_of(file, loop.type), "a written type on a candidate loop's variable");

    auto const visible = scope.locals.size();
    ++scope.depth;
    auto const* const n = ast::is_valid(loop.variable) ? ast.at(loop.variable).node.try_as<ast::name>() : nullptr;
    if (n != nullptr)
    {
        auto const self = target{.kind = target_kind::local, .index = i32(id)};
        set_type(file, loop.variable, candidate);
        set_target(file, loop.variable, self);
        judge_shadowing(file, text_of(file, n->where), n->where);
        declare_local(scope, {.name = text_of(file, n->where), .where = self, .type = candidate});
    }
    scope.loops.push_back({});
    (void)check_statements(scope, loop.body.statements);
    scope.loops.remove_back();
    --scope.depth;
    scope.locals.resize_down_to(visible);
    for (auto& q : scope.query_lets)
        if (q.loop == id)
            q.is_loop_open = false;
}
