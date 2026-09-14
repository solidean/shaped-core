#include "blocking_wait.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "blocking-wait";
constexpr cc::string_view k_rationale
    = "a blocking wait in a test holds a worker the run needs and can run other tests nested on its stack; a test "
      "awaits instead, and the files whose subject is the wait itself are allowed by name in a .shaped-lint.yml.";

void check(lint_context& ctx)
{
    if (!ctx.config.checks_blocking_waits())
        return;

    auto const path = cc::glob_normalize_path(ctx.source.path());

    for (auto const& t : ctx.tokens.tokens)
    {
        if (!t.is(token_kind::identifier))
            continue;

        auto const decision = ctx.config.classify_blocking_wait(path, t.text);
        if (decision.verdict != include_verdict::denied)
            continue;

        ctx.report({
            .rule_id = k_id,
            .span = t.span,
            .message = cc::format("{} blocks here", t.text),
            .sev = severity::warning,
            .suggested_hint = hint{.message = cc::string(decision.reason)},
        });
    }
}
} // namespace

rule const& blocking_wait_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::tokens,
        .languages = k_cpp_only,
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
