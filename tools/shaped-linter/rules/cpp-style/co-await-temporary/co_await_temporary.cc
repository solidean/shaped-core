#include "co_await_temporary.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "co-await-temporary";
constexpr cc::string_view k_rationale
    = "co_await hands back a reference into the awaited node, and a temporary handle dies at the end of the statement; "
      "when nothing else owns the node the reference dangles, so hold the async in a variable first. The callee names "
      "are a heuristic: allow by name a getter whose node an object keeps.";

/// The callee of the call that ends `sig`, the significant tokens of an awaited expression, or empty when it ends in none.
/// `name(...)`, `name<...>(...)` and a call of a lambda or a call's result count; `(...)` alone is a parenthesized expression.
cc::string_view callee_of(cc::span<token const* const> sig)
{
    if (sig.empty() || !sig.back()->is_punct(")"))
        return {};
    // the `(` matching the last `)`
    auto depth = 0;
    auto open = isize(-1);
    for (auto i = sig.size() - 1; i >= 0; --i)
    {
        if (sig[i]->is_punct(")"))
            ++depth;
        else if (sig[i]->is_punct("(") && --depth == 0)
        {
            open = i;
            break;
        }
    }
    if (open <= 0)
        return {};
    auto const* before = sig[open - 1];
    if (before->is(token_kind::identifier))
        return before->text;
    if (before->is_punct(">"))
    {
        // a template call: the identifier ahead of the matching `<`
        auto angle = 0;
        for (auto i = open - 1; i >= 0; --i)
        {
            if (sig[i]->is_punct(">"))
                ++angle;
            else if (sig[i]->is_punct("<") && --angle == 0)
                return i > 0 && sig[i - 1]->is(token_kind::identifier) ? sig[i - 1]->text : cc::string_view("<call>");
        }
        return {};
    }
    if (before->is_punct(")") || before->is_punct("]") || before->is_punct("}"))
        return "<call>";
    return {};
}

void check(lint_context& ctx)
{
    if (!ctx.config.checks_co_await_temporaries())
        return;

    auto const path = cc::glob_normalize_path(ctx.source.path());

    // the significant tokens, so the patterns below read across whitespace and comments
    auto sig = cc::vector<token const*>();
    for (auto const& t : ctx.tokens.tokens)
        if (!t.is_trivia())
            sig.push_back(&t);

    auto expr = cc::vector<token const*>();
    for (auto i = isize(3); i < sig.size(); ++i)
    {
        if (!sig[i]->is_keyword("co_await"))
            continue;
        // `& name = co_await` or `&& name = co_await`: a reference declarator, not an assignment to one
        if (!sig[i - 1]->is_punct("=") || !sig[i - 2]->is(token_kind::identifier)
            || !(sig[i - 3]->is_punct("&") || sig[i - 3]->is_punct("&&")))
            continue;

        expr.clear();
        auto depth = 0;
        for (auto j = i + 1; j < sig.size(); ++j)
        {
            auto const& t = *sig[j];
            if (depth == 0 && t.is_punct(";"))
                break;
            if (t.is_punct("(") || t.is_punct("[") || t.is_punct("{"))
                ++depth;
            else if (t.is_punct(")") || t.is_punct("]") || t.is_punct("}"))
                --depth;
            expr.push_back(&t);
        }
        auto const callee = callee_of(expr);
        if (callee.empty() || callee == "move" || callee == "forward")
            continue;

        auto const decision = ctx.config.classify_co_await_temporary(path, callee);
        if (decision.verdict != include_verdict::denied)
            continue;

        auto const name = sig[i - 2]->text;
        ctx.report({
            .rule_id = k_id,
            .span = {.file_id = sig[i]->span.file_id,
                     .byte_begin = sig[i]->span.byte_begin,
                     .byte_end = expr.back()->span.byte_end},
            .message
            = cc::format("`{}` refers into the node of a temporary async, which dies at the end of the statement", name),
            .sev = severity::warning,
            .suggested_hint = hint{.message = cc::format("hold the async first: `auto const {0}_async = ...; auto "
                                                         "const& {0} = "
                                                         "co_await {0}_async;`; {1}",
                                                         name, decision.reason)},
        });
    }
}
} // namespace

rule const& co_await_temporary_rule()
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
