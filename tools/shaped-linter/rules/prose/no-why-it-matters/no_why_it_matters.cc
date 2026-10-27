#include "no_why_it_matters.hh"

#include <clean-core/string/string.hh>
#include <rules/prose/impl/phrase_match.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "no-why-it-matters";
constexpr cc::string_view k_rationale
    = "announcing that something matters asks the reader to take its weight on trust; the consequence itself says "
      "more in fewer words: what breaks, what it costs, what it enables. "
      "This is a heuristic over phrase patterns: if one fires on a sentence that genuinely needs it, narrow the "
      "pattern in no_why_it_matters.cc rather than rewording around it.";

// The patterns, in phrase_match.hh's notation.
// Each is an announcement whose subject is the sentence before it, which is why a real subject keeps a sentence quiet:
// `the order matters` names what matters, `This matters` points back at it.
constexpr cc::string_view k_patterns[] = {
    "why this|that|it|they [really|still|also|even] matters|matter",
    "why this|that|it|they is|are [really|so] important",
    "why does|did|do|would|should this|that|it|they [really] matter",
    "^ here's|here why",
    "^ here is why",
    "^ this|that|it [really|also|still|only|all|especially] matters",
    ",|— which [really|also|still|especially] matters",
    "matters|matter [far|much] more than it|they [might|may] sounds|sound|looks|look|seems|seem",
    "this|that|which|here is the part|bit|thing that matters",
    "this's|that's|here's the part|bit|thing that matters",
    "what this|that means for you|us",
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
                .message = cc::string("announces that something matters instead of saying what follows from it"),
                .sev = severity::warning,
                .suggested_hint = hint{.message = cc::string("state the consequence directly, then drop the "
                                                             "announcement; a heading like this becomes the "
                                                             "consequence itself")},
            });
}
} // namespace

rule const& no_why_it_matters_rule()
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
