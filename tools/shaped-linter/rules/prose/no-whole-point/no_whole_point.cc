#include "no_whole_point.hh"

#include <clean-core/string/string.hh>
#include <rules/prose/impl/phrase_match.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "no-whole-point";
constexpr cc::string_view k_rationale
    = "calling a property the point asserts its importance instead of showing it; saying what the property buys "
      "(\"so a caller never ...\") is the same length and carries the reason. "
      "This is a heuristic over phrase patterns: if one fires on a literal point, narrow the pattern in "
      "no_whole_point.cc rather than rewording around it.";

// The patterns, in phrase_match.hh's notation.
// A literal point is never followed by a clause end, and is rarely the object of `is`, so most patterns demand one or the other.
constexpr cc::string_view k_patterns[] = {
    "the|its|their|our|this|whose whole|entire point !set|sets|cloud|clouds",
    "the very point $",
    "the very point of",
    "is|was|are|were|be|being [exactly|precisely|really|also|still] the point $",
    "which|that|this|it is|was [exactly|precisely|really|also] the point of",
    "that's|this's|which's [exactly|precisely] the point",
    "the [whole|real|main] point [here] is|was that",
    "the [whole|real|main] point [here] is|was to !the|a|an|its|this|that|one|either|both|each|left|right",
    "the point here is|was",
    "the crux",
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
                .message = cc::string("calls something the point instead of saying what it achieves"),
                .sev = severity::warning,
                .suggested_hint = hint{.message = cc::string("say what the property buys, often as a `so ...` "
                                                             "clause, and drop the declaration")},
            });
}
} // namespace

rule const& no_whole_point_rule()
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
