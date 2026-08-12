#include "engine.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/streams/file_stream.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/lex/python_lexer.hh>
#include <shaped-linter/lex/source_language.hh>
#include <shaped-linter/parse/parser.hh>
#include <shaped-linter/parse/syntax_tree.hh>
#include <shaped-linter/prose/prose_view.hh>

namespace scl
{
lint_config const& empty_lint_config()
{
    static lint_config const empty;
    return empty;
}

cc::vector<finding> run_rules(source_buffer const& buffer, cc::span<rule const> rules, lint_config const& config)
{
    cc::vector<finding> out;

    // The file's extension picks the front end, and rules that did not ask for this language never run.
    // This is the only place a language is decided; below it every layer is already language-correct.
    auto const language = language_from_path(buffer.path());

    cc::vector<rule> active;
    for (auto const& r : rules)
        if (r.check && applies_to(r, language))
            active.push_back(r);
    if (active.empty())
        return out;

    token_stream ts; // markdown has no lexer, and no rule asks it for tokens
    if (language == source_language::cpp || language == source_language::python)
    {
        auto tokens = language == source_language::cpp ? lex(buffer) : lex_python(buffer);
        if (tokens.has_error())
            return out; // lexing is effectively infallible; nothing to lint if it somehow fails
        ts = cc::move(tokens.value());
    }

    syntax_tree tree; // empty unless a rule needs it — and only C++ has a parser at all
    if (language == source_language::cpp && any_needs_tree(active))
    {
        auto parsed = parse(buffer, ts);
        if (parsed.has_value())
            tree = cc::move(parsed.value());
    }

    prose_view prose; // empty unless a rule walks prose
    if (any_needs_prose(active))
        prose = extract_prose(buffer, language, ts);

    lint_context ctx
        = {.source = buffer, .language = language, .tokens = ts, .tree = tree, .prose = prose, .config = config, .out = out};
    for (auto const& r : active)
        r.check(ctx);

    return out;
}

cc::vector<finding> run_rules_on_text(cc::string_view source, cc::string_view path, lint_config const& config)
{
    auto const buffer = source_buffer::from_text(cc::string(source), path, 0);
    return run_rules(buffer, all_rules(), config);
}

cc::result<cc::unit> write_file(cc::string_view path, cc::string_view content)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    CC_RETURN_IF_ERROR(adapter);
    auto stream = adapter.value().stream();
    CC_RETURN_IF_ERROR(stream.write(cc::as_bytes(content)));
    CC_RETURN_IF_ERROR(stream.flush()); // no auto-flush: buffered bytes are lost otherwise
    return cc::unit{};
}

cc::string apply_edits(cc::string_view original, cc::span<text_edit const> edits)
{
    // Apply highest-offset-first so earlier offsets stay valid.
    // Insertion sort of pointers by
    // descending begin, then by descending end — a file carries only a handful of edits.
    // The second key matters when an insertion (an empty span) shares its offset with a replacement that
    // starts there: the wider edit has to go first, or the overlap guard below sees the pair out of order.
    cc::vector<text_edit const*> ordered;
    for (auto const& e : edits)
        ordered.push_back(&e);
    auto const precedes = [](text_edit const& a, text_edit const& b)
    {
        if (a.span.byte_begin != b.span.byte_begin)
            return a.span.byte_begin < b.span.byte_begin;
        return a.span.byte_end < b.span.byte_end;
    };
    for (isize i = 1; i < ordered.size(); ++i)
        for (isize j = i; j > 0 && precedes(*ordered[j - 1], *ordered[j]); --j)
        {
            auto const* tmp = ordered[j - 1];
            ordered[j - 1] = ordered[j];
            ordered[j] = tmp;
        }

    auto text = cc::string(original);
    // Overlap guard: edits are descending, so each end must not exceed the previous begin.
    // Only read by
    // the assert, so it is stripped (with its reads) in release — mark it to avoid a set-but-unused warning.
    [[maybe_unused]] u32 prev_begin = u32(text.size()) + 1;
    for (auto const* e : ordered)
    {
        CC_ASSERT(e->span.byte_end <= prev_begin, "overlapping fix edits");
        prev_begin = e->span.byte_begin;

        auto const head = cc::string_view(text).subview({.start = 0, .end = isize(e->span.byte_begin)});
        auto const tail = cc::string_view(text).subview({.start = isize(e->span.byte_end), .end = text.size()});
        text = cc::string(head) + e->replacement + cc::string(tail);
    }
    return text;
}

cc::vector<text_edit> collect_fix_edits(cc::span<finding const> findings)
{
    cc::vector<text_edit> edits;

    // Two equal insertions could not be caught by `apply_edits`' overlap guard — an empty span never
    // overlaps anything — so the duplicate has to be dropped here or the line lands once per finding.
    auto const already_collected = [&](text_edit const& e)
    {
        for (auto const& seen : edits)
            if (seen.span.file_id == e.span.file_id && seen.span.byte_begin == e.span.byte_begin
                && seen.span.byte_end == e.span.byte_end && seen.replacement == e.replacement)
                return true;
        return false;
    };

    for (auto const& f : findings)
    {
        // Only `suggested_fix`. A finding's `suggested_hint` is never read here — that is the whole
        // distinction between the two, so it is enforced by this loop and nothing else.
        if (!f.suggested_fix.has_value())
            continue;
        for (auto const& e : f.suggested_fix.value().edits)
            if (!already_collected(e))
                edits.push_back({.span = e.span, .replacement = e.replacement});
    }
    return edits;
}

cc::result<isize> apply_fixes(source_manager const& sm, cc::span<finding const> findings)
{
    auto const edits = collect_fix_edits(findings);

    cc::set<u32> files;
    for (auto const& e : edits)
        files.insert(e.span.file_id);

    isize changed = 0;
    for (u32 const fid : files)
    {
        cc::vector<text_edit> file_edits;
        for (auto const& e : edits)
            if (e.span.file_id == fid)
                file_edits.push_back({.span = e.span, .replacement = e.replacement});

        auto const text = apply_edits(sm.buffer(fid).text(), file_edits);
        CC_RETURN_IF_ERROR(write_file(sm.buffer(fid).path(), text));
        ++changed;
    }
    return changed;
}
} // namespace scl
