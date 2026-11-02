#include "no_stock_words.hh"

#include <clean-core/string/string.hh>
#include <rules/prose/impl/phrase_match.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "no-stock-words";
constexpr cc::string_view k_rationale
    = "some words have been worn into filler by overuse and now read as stock rather than meaning; say what the word "
      "gestured at instead: for `load-bearing`, what breaks without the thing. "
      "The list grows one decided entry at a time; a word whose plain sense is common in code never belongs on it.";

// The words, in phrase_match.hh's notation, one decided entry each with every spelling it takes.
// A hyphenated word wrapped at its hyphen reads as three tokens, `load - bearing`.
constexpr cc::string_view k_patterns[] = {
    "load-bearing",
    "load bearing",
    "load - bearing",
};

void check(lint_context& ctx)
{
    static auto const patterns = []
    {
        auto v = cc::vector<impl::phrase_pattern>{};
        for (auto const p : k_patterns)
            v.push_back(impl::compile_phrase(p));
        return v;
    }();

    for (auto const& block : ctx.prose.blocks)
        for (auto const& span : impl::find_phrases(block, patterns))
            ctx.report({
                .rule_id = k_id,
                .span = span,
                .message = cc::string("a word worn into filler by overuse"),
                .sev = severity::warning,
                .suggested_hint = hint{.message = cc::string("say what the word gestured at: for `load-bearing`, "
                                                             "what breaks or goes wrong without the thing")},
            });
}
} // namespace

rule const& no_stock_words_rule()
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
