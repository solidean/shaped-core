#include "no_flow_prose.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "no-flow-prose";
constexpr cc::string_view k_rationale
    = "prose is one semantic point per line, never a reflowed block: reading the first few words of each "
      "line has to give the shape of the passage. See docs/coding-guidelines.md, section "
      "\"Prose style - one semantic point per line\". "
      "This is a heuristic and an abbreviation ending in a dot is its false positive: if one fired on you, "
      "add the word to the abbreviation list in no_flow_prose.cc rather than rewording the line.";

// What the rule looks for, and why it is a heuristic rather than a test.
//
// A reflowed block is not decidable from text — "did this line break because the point ended, or because a
// column was reached?" is a question about intent.
// One shape, though, gives it away almost always: a
// sentence that ENDS in the middle of a line.
// Two short sentences on one line are fine when the line ends
// with the second one; a long sentence that had to wrap is fine because it carries no interior full stop.
// A `.` mid-line with more text behind it means two points were packed onto one line.
//
// Hence the whole detector: `<word>. <more text>`. Everything below is the exclusions that keep it usable,
// each earned on this tree:
//
//   - a word of one letter          `e.g. foo`, `i.e. bar` — by far the commonest false positive
//   - a word of only digits         `12. list item` — an ordered-list marker, not a sentence
//   - a known abbreviation          `etc.`, `incl.`, `vs.`, `cf.`, `resp.`, `al.`
//   - an odd number of backticks    inside inline code, where `f(x). y` is a code sample, not prose
//
// Known to survive, and accepted: an abbreviation outside the list, and a sentence ending on a quoted or
// parenthesized word (`… "done". Next`) whose closing punctuation sits between the word and the `.`.

bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/// Abbreviations whose trailing dot is not a sentence end.
/// One- and two-letter words are already excluded
/// by the length rule, so `e.g.` and `i.e.` are not listed here.
bool is_abbreviation(cc::string_view word)
{
    // Earned one at a time from a real false positive on this tree — `incl.` alone accounted for 13.
    static constexpr cc::string_view known[]
        = {"etc", "vs", "cf", "al", "resp", "approx", "fig", "vol", "incl", "excl"};
    for (auto const w : known)
        if (word == w)
            return true;
    return false;
}

/// The offset of the first sentence end in the middle of `line`, or -1 if it reads as one point.
isize interior_sentence_end(cc::string_view line)
{
    isize backticks = 0;

    for (isize i = 0; i < line.size(); ++i)
    {
        if (line[i] == '`')
        {
            ++backticks;
            continue;
        }
        if (line[i] != '.' || i == 0 || !is_alnum(line[i - 1]))
            continue;
        if (backticks % 2 != 0)
            continue; // inside inline code

        // A sentence end is a `.` followed by a space.
        // `1.5`, `foo.bar` and `…` are not.
        if (i + 1 >= line.size() || line[i + 1] != ' ')
            continue;

        // Something has to follow on this line, or the point genuinely ended it.
        auto rest = i + 2;
        while (rest < line.size() && (line[rest] == ' ' || line[rest] == '\t'))
            ++rest;
        if (rest >= line.size())
            continue;

        auto word_begin = i;
        while (word_begin > 0 && is_alnum(line[word_begin - 1]))
            --word_begin;
        auto const word = line.subview({.start = word_begin, .end = i});

        if (word.size() < 2)
            continue; // `e.g.`, `i.e.`, a lone initial
        if (is_abbreviation(word))
            continue;

        auto all_digits = true;
        for (auto const c : word)
            if (c < '0' || c > '9')
                all_digits = false;
        if (all_digits)
            continue; // an ordered-list marker

        return i;
    }

    return -1;
}

void check(lint_context& ctx)
{
    for (auto const& block : ctx.prose.blocks)
        for (auto const& line : block.lines)
        {
            auto const at = interior_sentence_end(line.text);
            if (at < 0)
                continue;

            // Underline the seam — the `.` and the space after it — so the carets sit where the break goes.
            auto const begin = line.span.byte_begin + u32(at);
            ctx.report({
                .rule_id = k_id,
                .span = {.file_id = line.span.file_id, .byte_begin = begin, .byte_end = begin + 2},
                .message = cc::string("a sentence ends mid-line; prose is one semantic point per line"),
                .sev = severity::warning,
                .suggested_hint = hint{.message = cc::string("break the line here, then re-read the whole block: the "
                                                             "fix is modelling the "
                                                             "prose, not inserting a newline. See "
                                                             "docs/coding-guidelines.md, section "
                                                             "\"Prose style - one semantic point per line\"")},
            });
        }
}
} // namespace

rule const& no_flow_prose_rule()
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
