#include "engine.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/streams/file_stream.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/parse/parser.hh>
#include <shaped-linter/parse/syntax_tree.hh>

namespace scl
{
cc::vector<finding> run_rules(source_buffer const& buffer, cc::span<rule const> rules)
{
    cc::vector<finding> out;

    auto tokens = lex(buffer);
    if (tokens.has_error())
        return out; // lexing is effectively infallible; nothing to lint if it somehow fails
    auto const ts = cc::move(tokens.value());

    syntax_tree tree; // empty unless a rule needs it
    if (any_needs_tree(rules))
    {
        auto parsed = parse(buffer, ts);
        if (parsed.has_value())
            tree = cc::move(parsed.value());
    }

    lint_context ctx = {.source = buffer, .tokens = ts, .tree = tree, .out = out};
    for (auto const& r : rules)
        if (r.check)
            r.check(ctx);

    return out;
}

cc::vector<finding> run_rules_on_text(cc::string_view source, cc::string_view path)
{
    auto const buffer = source_buffer::from_text(cc::string(source), path, 0);
    return run_rules(buffer, all_rules());
}

namespace
{
cc::result<cc::unit> write_file(cc::string_view path, cc::string_view content)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    CC_RETURN_IF_ERROR(adapter);
    auto stream = adapter.value().stream();
    CC_RETURN_IF_ERROR(stream.write(cc::as_bytes(content)));
    CC_RETURN_IF_ERROR(stream.flush()); // no auto-flush: buffered bytes are lost otherwise
    return cc::unit{};
}
} // namespace

cc::string apply_edits(cc::string_view original, cc::span<text_edit const> edits)
{
    // Apply highest-offset-first so earlier offsets stay valid. Insertion sort of pointers by
    // descending begin — a file carries only a handful of edits.
    cc::vector<text_edit const*> ordered;
    for (auto const& e : edits)
        ordered.push_back(&e);
    for (isize i = 1; i < ordered.size(); ++i)
        for (isize j = i; j > 0 && ordered[j - 1]->span.byte_begin < ordered[j]->span.byte_begin; --j)
        {
            auto const* tmp = ordered[j - 1];
            ordered[j - 1] = ordered[j];
            ordered[j] = tmp;
        }

    auto text = cc::string(original);
    // Overlap guard: edits are descending, so each end must not exceed the previous begin. Only read by
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

cc::result<isize> apply_fixes(source_manager const& sm, cc::span<finding const> findings)
{
    cc::vector<text_edit> edits;
    cc::set<u32> files;
    for (auto const& f : findings)
    {
        if (!f.suggested_fix.has_value())
            continue;
        for (auto const& e : f.suggested_fix.value().edits)
        {
            edits.push_back({.span = e.span, .replacement = e.replacement});
            files.insert(e.span.file_id);
        }
    }

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
