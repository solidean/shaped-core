#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Function bodies in a target's own text (the spec's checking file, CHK-427).

namespace
{
constexpr auto void_type = checked_module::void_type;
constexpr auto error_type = checked_module::error_type;

/// The language a tag names, among those a target writes; none for `sgl` and any other name.
cc::optional<shading_language> language_named(cc::string_view tag)
{
    for (auto i = isize(1); i < isize(sizeof(k_shading_language_names) / sizeof(k_shading_language_names[0])); ++i)
        if (k_shading_language_names[i] == tag)
            return shading_language(i);
    return {};
}
} // namespace

i32 checker::tagged_text_token(i32 file, ast::stmt_id stmt) const
{
    auto const& ast = ast_of(file);
    auto const* const e = ast.at(stmt).node.try_as<ast::expr_stmt>();
    if (e == nullptr || !ast::is_valid(e->value))
        return -1;
    auto const* const literal = ast.at(e->value).node.try_as<ast::literal>();
    if (literal == nullptr || literal->kind != ast::literal_kind::quoted)
        return -1;
    auto const& source = file_of(file);
    auto const open = index_of(source.at(ast.at(e->value).form).token);
    // a tagged opener is the only quote a name follows directly (STR-31)
    if (open < 0 || open + 1 >= source.tokens.size() || source.tokens[open + 1].kind != token_kind::symbol)
        return -1;
    return i32(open);
}

void checker::plan_target_text(symbol_id function, i32 file, ast::range_of<ast::stmt_id> statements)
{
    auto const& ast = ast_of(file);
    auto const list = cc::vector<ast::stmt_id>::create_copy_of(ast.at(statements));
    if (list.empty())
        return;
    auto is_text = true;
    for (auto const s : list)
        is_text = is_text && tagged_text_token(file, s) >= 0;
    if (is_text)
        return register_text_run(function, file, list);
    if (list.size() != 1)
        return;
    auto const* const chain = ast.at(list[0]).node.try_as<ast::if_stmt>();
    if (chain == nullptr || !chain->is_const)
        return;
    for (auto const& branch : ast.at(chain->branches))
        plan_target_text(function, file, branch.then.statements);
}

void checker::judge_text_effects(symbol_id function, i32 file, ast::range_of<ast::attribute> attributes)
{
    auto has_text = false;
    for (auto i = isize(0); i < out.target_texts.size(); ++i)
        has_text = has_text || out.target_texts[i].function == function;
    cc::string_view const declared[] = {"barrier", "subgroup", "derivatives"};
    for (auto const name : declared)
        if (auto const* const a = find_attribute(file, attributes, name); a != nullptr && !has_text)
            unsupported(file, a->name,
                        cc::format("@{} on a function whose body is SGL, which says what it does itself; it declares "
                                   "what a body in a target's own text does",
                                   name));
    auto const* const barrier = find_attribute(file, attributes, "barrier");
    if (barrier != nullptr && find_attribute(file, attributes, "pure") != nullptr)
        report(diagnostic_kind::invalid_attribute_arguments, file, barrier->name,
               "a barrier is an effect, so a @barrier function is never @pure");
}

void checker::register_text_run(symbol_id function, i32 file, cc::span<ast::stmt_id const> run)
{
    auto const& ast = ast_of(file);
    auto const& source = file_of(file);
    auto const& tokens = source.tokens;
    auto const& info = out.functions[out.at(function).info];
    auto const parameters = cc::vector<parameter>::create_copy_of(out.at(info.parameters));
    auto const has_result = is_valid(info.result) && info.result != void_type && info.result != error_type;
    auto const& name = out.at(function).name;

    for (auto const& p : parameters)
        if (text_of(file, ast.at(p.field).name) == "result")
            report(diagnostic_kind::invalid_target_text, file, ast.at(p.field).name,
                   cc::format("{} has a body in a target's text, whose `$result` is the value it gives, so no "
                              "parameter of it is named result",
                              name));

    auto languages = cc::vector<shading_language>();
    for (auto k = isize(0); k < run.size(); ++k)
    {
        auto const open = tagged_text_token(file, run[k]);
        text_statements.push_back({.file = file, .stmt = run[k], .ends_run = k + 1 == run.size()});
        auto const tag_span = tokens[open + 1].where;
        auto const tag = source.text_of(tag_span);
        auto const language = language_named(tag);
        if (!language.has_value())
        {
            report(diagnostic_kind::invalid_target_text, file, tag_span,
                   cc::format("{} names no language a target writes; a body's text is one of hlsl, wgsl or msl", tag));
            continue;
        }
        if (languages.values().contains(language.value()))
        {
            report(diagnostic_kind::invalid_target_text, file, tag_span,
                   cc::format("{} has a second body in {}, where one per language is all a target takes", name, tag));
            continue;
        }
        languages.push_back(language.value());

        // the text between the placeholders, each line ending where its line in the source does
        auto text = target_text{.function = function, .file = file, .first = run[0], .language = language.value()};
        text.pieces.push_back({});
        auto has_result_slot = false;
        auto is_sound = true;
        auto previous_end = tag_span.end();
        auto has_content = false;
        for (auto i = isize(open + 2); i < tokens.size() && tokens[i].kind != token_kind::quote_close; ++i)
        {
            auto const& t = tokens[i];
            auto const between = source.text_of({.offset = previous_end, .length = t.where.offset - previous_end});
            if (has_content)
                for (auto const c : between)
                    if (c == '\n')
                        text.pieces.back() += '\n';
            if (!has_content)
                text.offset = t.where.offset;
            previous_end = t.where.end();
            if (t.kind == token_kind::string_body)
            {
                // `$$` is a literal dollar; nothing else is escaped in a multi-line string (STR-14)
                auto const body = source.text_of(t.where);
                for (auto c = isize(0); c < body.size(); ++c)
                {
                    text.pieces.back() += body[c];
                    if (body[c] == '$' && c + 1 < body.size() && body[c + 1] == '$')
                        ++c;
                }
                has_content = has_content || !body.empty();
                continue;
            }
            if (t.kind != token_kind::dollar)
                continue;
            has_content = true;
            auto const is_name = i + 1 < tokens.size() && tokens[i + 1].kind == token_kind::symbol;
            if (!is_name)
            {
                report(diagnostic_kind::invalid_target_text, file, t.where,
                       "a placeholder of a target's text names a parameter or `result`: `$name`, or `$name.member`");
                is_sound = false;
                continue;
            }
            ++i;
            previous_end = tokens[i].where.end();
            auto const placeholder = source.text_of(tokens[i].where);
            auto slot = -2;
            if (placeholder == "result")
            {
                slot = -1;
                has_result_slot = true;
                if (!has_result)
                    report(diagnostic_kind::invalid_target_text, file, tokens[i].where,
                           cc::format("{} gives nothing, so its text has no `$result` to assign", name));
            }
            else
                for (auto p = isize(0); p < parameters.size(); ++p)
                    if (text_of(file, ast.at(parameters[p].field).name) == placeholder)
                        slot = i32(p);
            if (slot == -2)
            {
                report(diagnostic_kind::invalid_target_text, file, tokens[i].where,
                       cc::format("${} names no parameter of {}", placeholder, name));
                is_sound = false;
                continue;
            }
            text.slots.push_back(slot);
            text.pieces.push_back({});
            // `$h.x`: what follows is the target's member of the argument, which a target writes as a primary
            while (i + 2 < tokens.size() && tokens[i + 1].kind == token_kind::dot
                   && tokens[i + 2].kind == token_kind::symbol)
            {
                text.pieces.back() += '.';
                text.pieces.back() += source.text_of(tokens[i + 2].where);
                i += 2;
                previous_end = tokens[i].where.end();
            }
        }
        if (has_result && !has_result_slot)
        {
            report(diagnostic_kind::invalid_target_text, file, tag_span,
                   cc::format("{} gives a {}, which its {} text assigns to `$result`", name, out.name_of(info.result),
                              tag));
            is_sound = false;
        }
        if (is_sound)
            out.target_texts.push_back(cc::move(text));
    }
}

cc::optional<flow> checker::target_text_flow(i32 file, ast::stmt_id stmt)
{
    if (tagged_text_token(file, stmt) < 0)
        return {};
    for (auto const& t : text_statements)
        if (t.file == file && t.stmt == stmt)
            return t.ends_run ? flow::exits : flow::falls_through;
    report(diagnostic_kind::invalid_target_text, file, span_of(file, stmt),
           "a body in a target's own text stands only as a function's whole body, or as a whole branch of a const if "
           "that is that body");
    // as it would end its body where it stood rightly, so no missing return follows from it
    return flow::exits;
}

void checker::declare_preamble(i32 file, ast::preamble_decl const& preamble, cc::span<target_condition const> within)
{
    if (ast::is_valid(preamble.text))
    {
        auto text = plain_target_text(file, preamble.text, "a preamble");
        if (text.has_value())
            out.target_preambles.push_back({.file = file,
                                            .language = text.value().language,
                                            .text = cc::move(text.value().text),
                                            .condition = cc::vector<target_condition>::create_copy_of(within)});
        return;
    }
    // CHK-528: a branch holds where the chain's condition does, no branch before it held, and its own condition does
    auto const& ast = ast_of(file);
    auto earlier = cc::vector<i32>();
    auto nodes = cc::vector<target_condition>::create_copy_of(within);
    auto const outer = within.empty() ? -1 : i32(within.size()) - 1;
    for (auto const& branch : ast.at(preamble.branches))
    {
        auto condition = nodes;
        auto root = outer;
        auto const both = [&](i32 lhs, i32 rhs)
        {
            if (lhs < 0)
                return rhs;
            condition.push_back({.kind = target_condition::kind_t::both, .lhs = lhs, .rhs = rhs});
            return i32(condition.size()) - 1;
        };
        for (auto const e : earlier)
        {
            condition.push_back({.kind = target_condition::kind_t::negated, .lhs = e});
            root = both(root, i32(condition.size()) - 1);
        }
        if (ast::is_valid(branch.condition))
        {
            if (!target_condition_of(file, branch.condition, condition))
                return;
            auto const own = i32(condition.size()) - 1;
            earlier.push_back(own);
            root = both(root, own);
        }
        if (root < 0)
            condition.clear();
        else if (root != i32(condition.size()) - 1)
            condition.push_back({.kind = target_condition::kind_t::both, .lhs = root, .rhs = root});
        nodes = condition;
        for (auto const d : ast.at(branch.members))
        {
            auto const& inner = ast.at(d);
            if (auto const* const p = inner.node.try_as<ast::preamble_decl>())
                declare_preamble(file, *p, condition);
            else
                report(diagnostic_kind::invalid_target_text, file, span_of(file, inner.form),
                       "a file-scope const if holds preambles alone, tagged strings or const if chains of them");
        }
    }
}

cc::optional<checker::plain_text> checker::plain_target_text(i32 file, ast::expr_id expr, cc::string_view what)
{
    auto const& ast = ast_of(file);
    auto const& source = file_of(file);
    auto const& tokens = source.tokens;
    auto const open = index_of(source.at(ast.at(expr).form).token);
    auto const tag_span = tokens[open + 1].where;
    auto const tag = source.text_of(tag_span);
    auto const language = language_named(tag);
    if (!language.has_value())
    {
        report(diagnostic_kind::invalid_target_text, file, tag_span,
               cc::format("{} names no language a target writes; {} is one of hlsl, wgsl or msl", tag, what));
        return cc::nullopt;
    }

    // whole lines, as the target reads them
    auto result = plain_text{.language = language.value()};
    auto previous_end = tag_span.end();
    for (auto i = isize(open + 2); i < tokens.size() && tokens[i].kind != token_kind::quote_close; ++i)
    {
        auto const& t = tokens[i];
        auto const between = source.text_of({.offset = previous_end, .length = t.where.offset - previous_end});
        if (!result.text.empty() && between.contains('\n'))
            result.text += '\n';
        previous_end = t.where.end();
        if (t.kind == token_kind::dollar)
        {
            report(diagnostic_kind::invalid_target_text, file, t.where,
                   cc::format("{} belongs to no call, so it has no placeholder; `$$` is a dollar of the text", what));
            return cc::nullopt;
        }
        if (t.kind != token_kind::string_body)
            continue;
        auto const body = source.text_of(t.where);
        for (auto c = isize(0); c < body.size(); ++c)
        {
            result.text += body[c];
            if (body[c] == '$' && c + 1 < body.size() && body[c + 1] == '$')
                ++c;
        }
    }
    if (!result.text.empty())
        result.text += '\n';
    return result;
}
