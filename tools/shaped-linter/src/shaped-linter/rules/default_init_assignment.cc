#include "default_init_assignment.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "default-init-assignment";

// The rationale is Philip's stated reason, kept verbatim — the reporter prints it with every finding.
constexpr cc::string_view k_rationale
    = "prefer a consistent assignment-form initialization `T v = value;` across the codebase; every "
      "variable initializer — data member, function local, or namespace-scope variable — must therefore "
      "use `=`, not brace form.";

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// Trim leading/trailing ASCII whitespace.
cc::string_view trim(cc::string_view s)
{
    isize a = 0;
    isize b = s.size();
    while (a < b && is_space(s[a]))
        ++a;
    while (b > a && is_space(s[b - 1]))
        --b;
    return s.subview({.start = a, .end = b});
}

/// Whether the brace body has a comma at bracket depth 0 — the "multi-element / init-list" case that
/// must keep its braces. Depth counts `()`, `[]`, and `{}` only (NOT `<>`): a mistaken angle-depth
/// could hide a real top-level comma and make us drop braces around a list, producing invalid code, so
/// we err toward keeping braces. Scans the already-lexed tokens inside `inner`.
bool has_top_level_comma(token_stream const& ts, source_span inner)
{
    isize depth = 0;
    for (auto const& t : ts.tokens)
    {
        if (t.span.byte_begin < inner.byte_begin || t.span.byte_end > inner.byte_end)
            continue;
        if (t.is_trivia())
            continue;
        if (t.is_punct("(") || t.is_punct("[") || t.is_punct("{"))
            ++depth;
        else if (t.is_punct(")") || t.is_punct("]") || t.is_punct("}"))
            --depth;
        else if (t.is_punct(",") && depth == 0)
            return true;
    }
    return false;
}

/// Whether the first significant token inside the braces is `.` — a designated initializer, which must
/// keep its braces.
bool first_inner_is_designator(token_stream const& ts, source_span inner)
{
    for (auto const& t : ts.tokens)
    {
        if (t.span.byte_begin < inner.byte_begin || t.span.byte_end > inner.byte_end)
            continue;
        if (t.is_trivia())
            continue;
        return t.is_punct(".");
    }
    return false;
}

/// The replacement text (without the leading " = ") for a brace-form initializer:
///  - empty `{}`                    -> "{}"        (`int v{}`   -> `= {}`)
///  - top-level comma / designated  -> keep verbatim `{…}`   (`{a, b}` -> `= {a, b}`, `{.x=1}` kept)
///  - otherwise                     -> drop the braces, inner trimmed   (`{0}` -> `0`)
cc::string fix_payload(lint_context const& ctx, node const& v)
{
    auto const inner = trim(ctx.source.span_text(v.init_inner));
    if (inner.empty())
        return cc::string("{}");
    if (has_top_level_comma(ctx.tokens, v.init_inner) || first_inner_is_designator(ctx.tokens, v.init_inner))
        return cc::string(ctx.source.span_text(v.init_span)); // keep the whole `{…}` verbatim
    return cc::string(inner);                                 // drop braces
}

/// A data member reads differently from a local, so the finding says which one it is.
cc::string_view what_it_is(decl_scope scope)
{
    return scope == decl_scope::record_scope ? "member default initializer" : "variable initializer";
}

void check(lint_context& ctx)
{
    for (auto const& v : ctx.tree.nodes)
    {
        if (v.kind != node_kind::variable_declaration || v.form != init_form::brace)
            continue;

        auto payload = fix_payload(ctx, v);

        // Replace `name{…}` (from the end of the declarator-id through the closing brace) with `name = …`.
        auto const edit = text_edit{
            .span = {.file_id = v.name.file_id, .byte_begin = v.name.byte_end, .byte_end = v.init_span.byte_end},
            .replacement = cc::string(" = ") + payload,
        };

        ctx.report({
            .rule_id = k_id,
            .span = v.init_span,
            .message = cc::string(what_it_is(v.scope)) + " should use assignment form (`= value`), not brace form",
            .sev = severity::warning,
            .suggested_fix = fix{.edits = {edit}},
        });
    }
}
} // namespace

rule const& default_init_assignment_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::syntax_tree,
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
