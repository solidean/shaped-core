#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;

/// `text` without the spaces and tabs around it.
cc::string_view trimmed(cc::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text = text.subview({.start = 1, .end = text.size()});
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text = text.subview({.start = 0, .end = text.size() - 1});
    return text;
}
} // namespace

void checker::add_test(i32 file, ast::decl_id decl, cc::string scope_path, function_scope const* enclosing)
{
    auto const id = symbol_id(out.symbols.size());
    out.symbols.push_back({
        .file = file,
        .declaration = decl,
        .kind = symbol_kind::test,
        .state = symbol_state::checked,
        .info = i32(out.functions.size()),
    });
    // A test is checked as a function of no parameter and no binding that returns void (CHK-224).
    out.functions.push_back({
        .symbol = id,
        .parameters = {.first = u32(out.parameters.size()), .count = 0},
        .result = void_type,
        .bindings = {.first = u32(out.binding_lists.size()), .count = 0},
    });
    notes.push_back({});

    // Attributes stand in front of the keyword, so the keyword is where the report names the test.
    auto where = span_of(file, decl);
    auto const at = text_of(file, where).find("test");
    if (at >= 0)
        where = {.offset = where.offset + u32(at), .length = 4};
    auto const whole = span_of(file, decl);
    out.tests.push_back({
        .symbol = id,
        .file = file,
        .declaration = decl,
        .where = where,
        .extent = {.offset = where.offset, .length = whole.offset + whole.length - where.offset},
        .scope_path = cc::move(scope_path),
        .comment = comment_of(file, where),
    });

    auto captured = cc::vector<local_name>();
    if (enclosing != nullptr)
    {
        for (auto local : enclosing->captured)
            captured.push_back(local);
        for (auto local : enclosing->locals)
        {
            local.is_captured = true;
            captured.push_back(local);
        }
    }
    test_captures.push_back(cc::move(captured));
    test_enclosing.push_back(enclosing != nullptr ? enclosing->function : symbol_id::none);
}

void checker::add_member_tests(i32 file, ast::range_of<ast::decl_id> members, cc::string_view scope_path)
{
    for (auto const member : ast_of(file).at(members))
        if (ast_of(file).at(member).node.is<ast::test_decl>())
            add_test(file, member, cc::string(scope_path));
}

void checker::check_test(i32 index)
{
    // by value: checking the body appends to `out`, and the vectors behind these references move
    auto const test = out.tests[index];
    auto const file = test.file;
    auto const& ast = ast_of(file);
    auto const& d = ast.at(test.declaration);
    auto const& body = d.node.as<ast::test_decl>().body;
    auto const info = out.at(test.symbol).info;

    cc::string_view const known[] = {"expect"};
    judge_attributes(file, d.attributes, known, "a test");
    out.tests[index].expectations = expectations_of(file, d.attributes);
    notes[info].is_body_checked = true;
    auto scope = function_scope{
        .function = test.symbol,
        .file = file,
        .result = void_type,
        .is_test = true,
        .captured = test_captures[index],
        .enclosing = test_enclosing[index],
    };
    auto const errors_before = error_count();
    (void)check_statements(scope, body.statements);

    // CHK-226: a test that could pass without having checked anything is written as one that says so.
    auto const last = last_code_line(file, body.statements);
    auto const* const line = ast::is_valid(last) ? ast.at(last).node.try_as<ast::expr_stmt>() : nullptr;
    auto const type = line != nullptr ? out.files[file].type_at(line->value) : type_id::none;
    auto const bool_type = type_of_builtin(builtins::k_bool, file, test.where);
    if (type != error_type && (type != bool_type || !is_valid(type)))
        report(diagnostic_kind::test_must_end_in_check, file, ast::is_valid(last) ? span_of(file, last) : test.where,
               "the last line of a test is a check; end in `true // why` where its asserts are what it checks");

    notes[info].is_body_sound = error_count() == errors_before;
}

cc::vector<test_expectation> checker::expectations_of(i32 file, ast::range_of<ast::attribute> attributes)
{
    auto const& ast = ast_of(file);
    auto result = cc::vector<test_expectation>();
    for (auto const& a : ast.at(attributes))
    {
        if (text_of(file, a.name) != "expect")
            continue;
        auto const arguments = ast.at(a.arguments);
        if (arguments.empty())
            report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
                   "@expect names what the test does: .fail, .assert, error = \"kind\" or warning = \"kind\"");
        for (auto const& argument : arguments)
        {
            auto const where = ast::is_valid(argument.value) ? span_of(file, argument.value) : a.name;
            auto const* const dot
                = ast::is_valid(argument.value) ? ast.at(argument.value).node.try_as<ast::leading_dot>() : nullptr;
            auto const* const literal
                = ast::is_valid(argument.value) ? ast.at(argument.value).node.try_as<ast::literal>() : nullptr;
            auto const name = argument.name.empty() ? cc::string_view() : text_of(file, argument.name);
            auto const case_name = dot != nullptr ? text_of(file, dot->name) : cc::string_view();

            // CHK-231: a run it fails, or a kind of diagnostic, with `*` for any run of characters
            if (name.empty() && (case_name == "fail" || case_name == "assert"))
                result.push_back(
                    {.kind = case_name == "fail" ? expectation_kind::fail : expectation_kind::assert_, .where = where});
            else if ((name == "error" || name == "warning") && literal != nullptr
                     && literal->kind == ast::literal_kind::quoted)
            {
                auto pattern = text_of(file, where);
                if (pattern.size() >= 2)
                    pattern = pattern.subview({.offset = 1, .size = pattern.size() - 2});
                result.push_back({.kind = name == "error" ? expectation_kind::error : expectation_kind::warning,
                                  .pattern = cc::string(pattern),
                                  .where = where});
            }
            else
                report(diagnostic_kind::invalid_attribute_arguments, file, where,
                       "@expect names what the test does: .fail, .assert, error = \"kind\" or warning = \"kind\"");
        }
    }
    return result;
}

ast::stmt_id checker::last_code_line(i32 file, ast::range_of<ast::stmt_id> statements) const
{
    auto const& ast = ast_of(file);
    auto const list = ast.at(statements);
    if (list.empty())
        return ast::stmt_id::none;
    auto const last = list[list.size() - 1];
    auto const deeper = [&](ast::range_of<ast::stmt_id> inner)
    {
        auto const found = last_code_line(file, inner);
        return ast::is_valid(found) ? found : last;
    };

    auto const& s = ast.at(last);
    if (auto const* const chain = s.node.try_as<ast::if_stmt>())
    {
        auto const branches = ast.at(chain->branches);
        return branches.empty() ? last : deeper(branches[branches.size() - 1].then.statements);
    }
    if (auto const* const loop = s.node.try_as<ast::for_stmt>())
        return deeper(loop->body.statements);
    if (auto const* const loop = s.node.try_as<ast::while_stmt>())
        return deeper(loop->body.statements);
    if (auto const* const e = s.node.try_as<ast::expr_stmt>(); e != nullptr && ast::is_valid(e->value))
    {
        auto const& value = ast.at(e->value);
        if (auto const* const loop = value.node.try_as<ast::loop_expr>())
            return deeper(loop->body.statements);
        if (auto const* const c = value.node.try_as<ast::case_expr>())
        {
            auto const arms = ast.at(c->arms);
            return arms.empty() ? last : deeper(arms[arms.size() - 1].result.statements);
        }
    }
    return last;
}

cc::string checker::comment_of(i32 file, source_span where) const
{
    auto const source = cc::string_view(file_of(file).source);
    auto const at = isize(where.offset);
    auto begin = at;
    while (begin > 0 && source[begin - 1] != '\n')
        --begin;
    auto end = at;
    while (end < source.size() && source[end] != '\n')
        ++end;

    // on the `test` line itself, behind the code
    auto const line = source.subview({.start = at, .end = end});
    if (auto const slashes = line.find("//"); slashes >= 0)
        return cc::string(trimmed(line.subview({.start = slashes + 2, .end = line.size()})));

    // alone on the line above
    if (begin == 0)
        return {};
    auto above_begin = begin - 1;
    while (above_begin > 0 && source[above_begin - 1] != '\n')
        --above_begin;
    auto const above = trimmed(source.subview({.start = above_begin, .end = begin - 1}));
    if (above.starts_with("//"))
        return cc::string(trimmed(above.subview({.start = 2, .end = above.size()})));
    return {};
}

void checker::report_capture(function_scope const& scope, source_span where, local_name const& local)
{
    auto const enclosing = is_valid(scope.enclosing) ? cc::string_view(out.at(scope.enclosing).name) : "";
    auto& d = report(diagnostic_kind::test_captures_runtime_value, scope.file, where,
                     cc::format("{} is a value of {} when it runs, and a test runs on its own", local.name, enclosing));
    // The declaration it would read, which only a run of the function gives a value.
    auto const declared = local.where.kind == target_kind::local
                            ? span_of(scope.file, ast::stmt_id(local.where.index))
                            : ast_of(scope.file).at(ast::field_id(local.where.index)).name;
    d.notes.push_back({.file = scope.file, .where = declared, .message = cc::format("{} is declared here", local.name)});
}
