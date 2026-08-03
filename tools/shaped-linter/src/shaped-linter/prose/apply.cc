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

/// Append a plan diagnostic — already `error: `-prefixed and carrying its own site, since it is read by
/// the plan's author rather than debugged.
void check_code_unchanged(planned_rewrite const& rewritten,
                          cc::string_view original,
                          source_language language,
                          cc::vector<cc::string>& out)
{
    auto const before_buffer = source_buffer::from_text(cc::string(original), rewritten.path, 0);
    auto const after_buffer = source_buffer::from_text(cc::string(rewritten.text), rewritten.path, 0);

    auto before = lex_as(before_buffer, language);
    auto after = lex_as(after_buffer, language);
    if (before.has_error() || after.has_error())
    {
        out.push_back(
            cc::format("error: {}: cannot lex this file, so the edit cannot be checked against it", rewritten.path));
        return;
    }

    auto const old_tokens = significant_tokens(before.value());
    auto const new_tokens = significant_tokens(after.value());

    auto const shared = old_tokens.size() < new_tokens.size() ? old_tokens.size() : new_tokens.size();
    for (isize i = 0; i < shared; ++i)
    {
        if (old_tokens[i].kind == new_tokens[i].kind && comparable_text(old_tokens[i]) == comparable_text(new_tokens[i]))
            continue;

        auto const at = after_buffer.line_col_at(new_tokens[i].span.byte_begin);
        out.push_back(cc::format("error: {}:{}: this edit changes code, not just prose ('{}' where '{}' was)",
                                 rewritten.path, at.line, comparable_text(new_tokens[i]), comparable_text(old_tokens[i])));
        return;
    }

    if (old_tokens.size() != new_tokens.size())
        out.push_back(cc::format("error: {}: this edit changes code, not just prose ({} code tokens where there were "
                                 "{})",
                                 rewritten.path, new_tokens.size(), old_tokens.size()));
}

void collect_new_prose(planned_rewrite const& rewritten, source_manager& sources, cc::vector<finding>& out)
{
    cc::vector<rule> prose_rules;
    for (auto const& r : all_rules())
        if (r.layer == rule_layer::prose)
            prose_rules.push_back(r);
    if (prose_rules.empty())
        return;

    // Registered rather than local, so the findings' spans still resolve once this returns.
    auto const& buffer = sources.add_from_text(cc::string(rewritten.text), rewritten.path);
    for (auto& f : run_rules(buffer, prose_rules))
    {
        auto const line = buffer.line_col_at(f.span.byte_begin).line;

        auto written_here = false;
        for (auto const l : rewritten.edited_lines)
            if (l == line)
                written_here = true;
        if (!written_here)
            continue; // a violation the plan did not write is not the plan's to answer for

        out.push_back(cc::move(f));
    }
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

void validate_rewrite(planned_rewrite const& rewritten,
                      cc::string_view original,
                      source_manager& sources,
                      apply_problems& out)
{
    auto const language = language_from_path(rewritten.path);
    if (language != source_language::markdown)
        check_code_unchanged(rewritten, original, language, out.errors);

    // Runs even after a code-changed error: the point is to report everything wrong in one pass.
    collect_new_prose(rewritten, sources, out.findings);
}

apply_outcome apply_prose_plan(prose_plan const& plan, cc::string_view root, apply_settings const& settings)
{
    apply_outcome out;

    cc::vector<planned_rewrite> rewrites;
    cc::vector<cc::string> targets;
    cc::vector<file_prose_delta> deltas;
    auto edits = isize(0);

    // Everything is built and judged before anything is written, so a plan with a problem in its last file
    // leaves the earlier ones exactly as they were.
    // A failing file is recorded and skipped rather than returned on, so one run reports every problem.
    for (auto const& file : plan.files)
    {
        auto const target = join_path(root, file.path);

        source_manager sm;
        auto buffer = sm.add_from_file(target);
        if (buffer.has_error())
        {
            out.problems.errors.push_back(cc::format("error: cannot read {}", target));
            continue;
        }

        auto rewritten = build_rewrite(file, buffer.value()->text());
        if (rewritten.has_error())
        {
            out.problems.errors.push_back(rewritten.error().to_string()); // already `error: `-prefixed
            continue;
        }

        validate_rewrite(rewritten.value(), buffer.value()->text(), out.sources, out.problems);

        if (settings.stats)
            deltas.push_back({
                .path = file.path,
                .before = measure_file_prose(buffer.value()->text(), file.path),
                .after = measure_file_prose(rewritten.value().text, file.path),
            });

        rewrites.push_back(cc::move(rewritten.value()));
        targets.push_back(target);
        edits += file.edits.size();
    }

    if (!out.problems.empty())
        return out; // nothing is written when anything is wrong

    out.report = {.files_changed = rewrites.size(), .edits_applied = edits, .prose = cc::move(deltas)};
    if (settings.dry_run)
        return out;

    for (isize i = 0; i < rewrites.size(); ++i)
        if (auto const written = write_file(targets[i], rewrites[i].text); written.has_error())
            out.problems.errors.push_back(written.error().to_string());

    return out;
}
} // namespace scl
