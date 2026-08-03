#include "no_long_prose_line.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "no-long-prose-line";

/// The guidelines' hard ceiling, in characters.
constexpr isize k_max_chars = 200;

constexpr cc::string_view k_rationale
    = "line length in prose is free — typically 20-150 characters, whatever the point needs — but 200 is the "
      "hard ceiling: a point that long almost always holds two, and wants splitting at the seam rather than "
      "wrapping. See docs/coding-guidelines.md, section \"Prose style - one semantic point per line\". "
      "A line whose longest unbreakable run is itself over the ceiling (a bare URL, a long path) is left "
      "alone, since no split can bring it under.";

// Why this is its own rule rather than part of no-flow-prose.
//
// no-flow-prose finds a reflowed block by its tell: a sentence that ends in the middle of a line.
// A line can break the same guideline with no interior full stop anywhere — one enormous sentence that
// simply runs on, or a single point padded with clauses — and that shape is invisible to it.
// The ceiling is what catches it, and it is a flat measurement rather than a heuristic, so it carries none
// of no-flow-prose's false-positive caveats.
//
// Characters, not bytes.
// This tree's prose is full of em dashes and ellipses, each three bytes in UTF-8, so a byte count would
// report a comfortable 190-character line as over the ceiling.

bool is_continuation_byte(char c)
{
    return (u8(c) & 0xC0) == 0x80;
}

isize char_count(cc::string_view s)
{
    auto n = isize(0);
    for (auto const c : s)
        if (!is_continuation_byte(c))
            ++n;
    return n;
}

/// The longest run of non-blank characters.
/// A line whose longest run already exceeds the ceiling cannot be split under it, however it is broken.
isize longest_unbreakable_run(cc::string_view s)
{
    auto best = isize(0);
    auto run = isize(0);
    for (auto const c : s)
    {
        if (c == ' ' || c == '\t')
        {
            run = 0;
            continue;
        }
        if (is_continuation_byte(c))
            continue;
        ++run;
        if (run > best)
            best = run;
    }
    return best;
}

/// The byte offset of character `n`, or the whole size if `s` has fewer than that.
isize byte_offset_of_char(cc::string_view s, isize n)
{
    if (n <= 0)
        return 0;

    auto chars = isize(0);
    for (isize i = 0; i < s.size(); ++i)
    {
        if (is_continuation_byte(s[i]))
            continue;
        if (chars == n)
            return i;
        ++chars;
    }
    return s.size();
}

void check(lint_context& ctx)
{
    for (auto const& block : ctx.prose.blocks)
        for (auto const& line : block.lines)
        {
            // The marker is stripped from `text`, so the line as written is its indent plus what is left.
            auto const total = isize(line.indent) + char_count(line.text);
            if (total <= k_max_chars)
                continue;
            if (longest_unbreakable_run(line.text) > k_max_chars)
                continue;

            // Underline the overhang alone — everything past the ceiling — so the carets show what is over.
            auto const overflow_at = byte_offset_of_char(line.text, k_max_chars - isize(line.indent));
            ctx.report({
                .rule_id = k_id,
                .span = {.file_id = line.span.file_id,
                         .byte_begin = line.span.byte_begin + u32(overflow_at),
                         .byte_end = line.span.byte_end},
                .message = cc::format("prose line is {} characters, over the {}-character ceiling", total, k_max_chars),
                .sev = severity::warning,
                .suggested_hint = hint{.message = cc::string("a point this long almost always holds two: split it at "
                                                             "the seam rather than wrapping it. See "
                                                             "docs/coding-guidelines.md, section "
                                                             "\"Prose style - one semantic point per line\"")},
            });
        }
}
} // namespace

rule const& no_long_prose_line_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::prose,
        .languages = k_all_languages,
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
