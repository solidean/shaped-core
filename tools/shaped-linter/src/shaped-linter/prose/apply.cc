#include "apply.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/lex/python_lexer.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_language.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/rules/engine.hh>
#include <shaped-linter/rules/registry.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
namespace
{
bool ends_with_newline(cc::string_view text)
{
    return !text.empty() && text.back() == '\n';
}

/// The line terminator the file already uses, taken from its first break.
/// A rewrite must not splice LF lines into a CRLF file — the result is a mixed file and a diff full of
/// lines nobody edited.
cc::string_view line_terminator(cc::string_view text)
{
    auto const first = text.find('\n');
    if (first > 0 && text[first - 1] == '\r')
        return "\r\n";
    return "\n";
}

/// The spelling two token sequences are compared by.
///
/// A preprocessor directive is lexed opaquely up to a trailing `//`, so its token text swallows the run of
/// spaces before that comment.
/// Trimming here is what lets a plan re-align a trailing comment — the directive itself still has to match
/// byte for byte.
cc::string_view comparable_text(token const& t)
{
    if (t.kind != token_kind::preprocessor_directive)
        return t.text;

    auto end = t.text.size();
    while (end > 0 && (t.text[end - 1] == ' ' || t.text[end - 1] == '\t'))
        --end;
    return t.text.subview({.start = 0, .end = end});
}

cc::vector<token> significant_tokens(token_stream const& ts)
{
    cc::vector<token> out;
    for (auto const& t : ts.all())
        if (!t.is_trivia())
            out.push_back(t);
    return out;
}

cc::result<token_stream> lex_as(source_buffer const& buffer, source_language language)
{
    return language == source_language::python ? lex_python(buffer) : lex(buffer);
}

cc::result<cc::unit> check_code_unchanged(planned_rewrite const& rewritten,
                                          cc::string_view original,
                                          source_language language)
{
    auto const before_buffer = source_buffer::from_text(cc::string(original), rewritten.path, 0);
    auto const after_buffer = source_buffer::from_text(cc::string(rewritten.text), rewritten.path, 0);

    auto before = lex_as(before_buffer, language);
    CC_RETURN_IF_ERROR(before);
    auto after = lex_as(after_buffer, language);
    CC_RETURN_IF_ERROR(after);

    auto const old_tokens = significant_tokens(before.value());
    auto const new_tokens = significant_tokens(after.value());

    auto const shared = old_tokens.size() < new_tokens.size() ? old_tokens.size() : new_tokens.size();
    for (isize i = 0; i < shared; ++i)
    {
        if (old_tokens[i].kind == new_tokens[i].kind && comparable_text(old_tokens[i]) == comparable_text(new_tokens[i]))
            continue;

        auto const at = after_buffer.line_col_at(new_tokens[i].span.byte_begin);
        return cc::error(cc::format("{}:{}: this edit changes code, not just prose ('{}' where '{}' was)", rewritten.path,
                                    at.line, comparable_text(new_tokens[i]), comparable_text(old_tokens[i])));
    }

    if (old_tokens.size() != new_tokens.size())
        return cc::error(cc::format("{}: this edit changes code, not just prose ({} code tokens where there were {})",
                                    rewritten.path, new_tokens.size(), old_tokens.size()));

    return cc::unit{};
}

cc::result<cc::unit> check_new_prose(planned_rewrite const& rewritten)
{
    cc::vector<rule> prose_rules;
    for (auto const& r : all_rules())
        if (r.layer == rule_layer::prose)
            prose_rules.push_back(r);
    if (prose_rules.empty())
        return cc::unit{};

    auto const buffer = source_buffer::from_text(cc::string(rewritten.text), rewritten.path, 0);
    for (auto const& f : run_rules(buffer, prose_rules))
    {
        auto const line = buffer.line_col_at(f.span.byte_begin).line;

        auto written_here = false;
        for (auto const l : rewritten.edited_lines)
            if (l == line)
                written_here = true;
        if (!written_here)
            continue; // a violation the plan did not write is not the plan's to answer for

        return cc::error(cc::format("{}:{}: {} ({})", rewritten.path, line, f.message, f.rule_id));
    }

    return cc::unit{};
}

/// The prose `text` carries, read as the language `path` names.
///
/// A file that will not lex measures as empty instead of failing: the numbers are a report, never a gate,
/// and the plan's own validation is what rejects a bad rewrite.
prose_stats measure_text(cc::string_view text, cc::string_view path)
{
    auto const language = language_from_path(path);
    auto const buffer = source_buffer::from_text(cc::string(text), path, 0);

    token_stream tokens; // markdown has no lexer and needs none
    if (language != source_language::markdown)
    {
        auto lexed = lex_as(buffer, language);
        if (lexed.has_error())
            return {};
        tokens = cc::move(lexed.value());
    }

    return measure_prose(extract_prose(buffer, language, tokens));
}

cc::string join_path(cc::string_view root, cc::string_view path)
{
    if (root.empty())
        return cc::string(path);
    return cc::string(root) + "/" + cc::string(path);
}
} // namespace

cc::result<planned_rewrite> build_rewrite(plan_file const& file, cc::string_view original)
{
    auto const buffer = source_buffer::from_text(cc::string(original), file.path, 0);
    auto const line_count = buffer.line_count();
    auto const trailing_newline = ends_with_newline(original);
    auto const eol = line_terminator(original);

    planned_rewrite out = {.path = file.path};
    cc::vector<text_edit> edits;

    // New-file line numbers run ahead of old ones by however many lines the earlier edits added or removed.
    // Edits are ascending and non-overlapping (the parser guarantees both), so one running delta is exact.
    auto delta = i64(0);

    for (auto const& e : file.edits)
    {
        if (e.is_insertion && e.first_line > line_count + 1)
            return cc::error(cc::format("{}: cannot insert before line {}, the file has {} lines", file.path,
                                        e.first_line, line_count));
        if (!e.is_insertion && e.last_line > line_count)
            return cc::error(cc::format("{}: span [{}-{}] runs past the end of the file, which has {} lines", file.path,
                                        e.first_line, e.last_line, line_count));

        auto const past_end = e.first_line > line_count;
        auto const begin = past_end ? u32(original.size()) : buffer.line_span(e.first_line).byte_begin;
        auto const end = e.is_insertion           ? begin
                       : e.last_line < line_count ? buffer.line_span(e.last_line + 1).byte_begin
                                                  : u32(original.size());

        // A replacement that reaches EOF must not invent a trailing newline the file did not have.
        auto const keep_last_newline = e.is_insertion || end < u32(original.size()) || trailing_newline;

        cc::string replacement;
        if (past_end && !trailing_newline && !original.empty())
            replacement += eol; // appending after a file that ends mid-line needs the break first
        for (isize i = 0; i < e.lines.size(); ++i)
        {
            replacement += e.lines[i];
            if (i + 1 < e.lines.size() || keep_last_newline)
                replacement += eol;
        }

        edits.push_back(
            {.span = {.file_id = 0, .byte_begin = begin, .byte_end = end}, .replacement = cc::move(replacement)});

        auto const new_first = u32(i64(e.first_line) + delta);
        for (isize i = 0; i < e.lines.size(); ++i)
            out.edited_lines.push_back(new_first + u32(i));
        delta += i64(e.lines.size()) - i64(e.removed_line_count());
    }

    out.text = apply_edits(original, edits);
    return out;
}

cc::result<cc::unit> validate_rewrite(planned_rewrite const& rewritten, cc::string_view original)
{
    auto const language = language_from_path(rewritten.path);
    if (language != source_language::markdown)
        CC_RETURN_IF_ERROR(check_code_unchanged(rewritten, original, language));

    return check_new_prose(rewritten);
}

cc::result<apply_report> apply_prose_plan(prose_plan const& plan, cc::string_view root, apply_settings const& settings)
{
    cc::vector<planned_rewrite> rewrites;
    cc::vector<cc::string> targets;
    cc::vector<file_prose_delta> deltas;
    auto edits = isize(0);

    // Everything is built and judged before anything is written, so a plan that fails on its last file
    // leaves the earlier ones exactly as they were.
    for (auto const& file : plan.files)
    {
        auto const target = join_path(root, file.path);

        source_manager sm;
        auto buffer = sm.add_from_file(target);
        if (buffer.has_error())
            return cc::error(cc::format("cannot read {}: {}", target, buffer.error().to_string()));

        auto rewritten = build_rewrite(file, buffer.value()->text());
        CC_RETURN_IF_ERROR(rewritten);
        CC_RETURN_IF_ERROR(validate_rewrite(rewritten.value(), buffer.value()->text()));

        if (settings.stats)
            deltas.push_back({
                .path = file.path,
                .before = measure_text(buffer.value()->text(), file.path),
                .after = measure_text(rewritten.value().text, file.path),
            });

        rewrites.push_back(cc::move(rewritten.value()));
        targets.push_back(target);
        edits += file.edits.size();
    }

    auto report = apply_report{.files_changed = rewrites.size(), .edits_applied = edits, .prose = cc::move(deltas)};
    if (settings.dry_run)
        return report;

    for (isize i = 0; i < rewrites.size(); ++i)
        CC_RETURN_IF_ERROR(write_file(targets[i], rewrites[i].text));

    return report;
}
} // namespace scl
