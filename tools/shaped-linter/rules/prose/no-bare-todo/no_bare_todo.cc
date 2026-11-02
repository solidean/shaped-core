#include "no_bare_todo.hh"

#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "no-bare-todo";

constexpr cc::string_view k_rationale
    = "open work is an issue, findable with `dev.py issues known`, not a word in a comment: file one with `dev.py "
      "issues new` and write `issue: <id>` (or `TEMPORARY(<id>)` on a workaround); a note that leaves nothing open "
      "says what it says without the marker. See docs/guides/issue-tracking.md.";

constexpr cc::string_view k_markers[] = {"TODO", "FIXME", "HACK", "XXX"};

bool is_word_char(char c)
{
    return cc::is_alphanumeric(c) || c == '_';
}

void check(lint_context& ctx)
{
    for (auto const& block : ctx.prose.blocks)
    {
        for (auto const& line : block.lines)
        {
            auto const text = line.text;
            auto in_backticks = false;
            for (isize i = 0; i < text.size(); ++i)
            {
                if (text[i] == '`')
                {
                    in_backticks = !in_backticks;
                    continue;
                }
                if (in_backticks || (i > 0 && is_word_char(text[i - 1])))
                    continue;

                for (auto const marker : k_markers)
                {
                    auto const end = i + marker.size();
                    if (end > text.size() || text.subview({.start = i, .end = end}) != marker)
                        continue;
                    if (end < text.size() && is_word_char(text[end]))
                        continue;

                    ctx.report({
                        .rule_id = k_id,
                        .span = {.file_id = line.span.file_id,
                                 .byte_begin = line.span.byte_begin + u32(i),
                                 .byte_end = line.span.byte_begin + u32(end)},
                        .message = cc::format("`{}` names no issue", marker),
                        .sev = severity::warning,
                        .suggested_hint = hint{.message = cc::string("file the open work as an issue and write "
                                                                     "`issue: <id>`, or drop the marker if "
                                                                     "nothing is left open")},
                    });
                    break;
                }
            }
        }
    }
}
} // namespace

rule const& no_bare_todo_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::prose,
        .languages = u8(language_bit(source_language::cpp) | language_bit(source_language::python)),
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
