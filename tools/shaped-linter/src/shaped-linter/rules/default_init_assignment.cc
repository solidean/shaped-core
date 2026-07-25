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
      "use `=`, not brace form. Where the braces were doing real work — an explicit constructor, or a "
      "conversion that copy-initialization will not perform — name the type: `T v = T(value);`.";

/// The replacement text (without the leading " = ") for a brace-form initializer: the `{…}` verbatim,
/// always. `int v{}` -> `= {}`, `P p{a, b}` -> `= {a, b}`, `cc::atomic<int> x{0}` -> `= {0}`.
///
/// The braces STAY. Dropping them around a single value reads better, but it turns direct-list-init into
/// copy-init, and a syntax-only linter cannot tell when that is legal: `Box a{10}` on an aggregate has no
/// conversion from `int` to fall back on, and neither does an adapter whose constructor is explicit.
/// Keeping them makes the rewrite copy-LIST-init, which every aggregate and every implicit constructor
/// accepts. An explicit constructor still refuses it — there the rationale's `T v = T(value)` is the
/// escape hatch, and no automatic rewrite can decide that for you.
cc::string fix_payload(lint_context const& ctx, node const& v)
{
    return cc::string(ctx.source.span_text(v.init_span));
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

        // Replace `declarator{…}` (from the end of the whole declarator through the closing brace) with
        // `declarator = …`. Starting at the declarator-id instead would swallow an array bound.
        auto const edit = text_edit{
            .span
            = {.file_id = v.declarator.file_id, .byte_begin = v.declarator.byte_end, .byte_end = v.init_span.byte_end},
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
