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
///
/// That is why the nicer forms below are a `hint` and not part of this fix.
cc::string fix_payload(lint_context const& ctx, node const& v)
{
    return cc::string(ctx.source.span_text(v.init_span));
}

// ---- the hint: the form we actually want, which only a human can sign off on -------------------------
//
// `= {value}` is the safe rewrite, not the pretty one. Two better forms exist, and each is a judgement
// call, so both ride along as a hint that `--fix` never applies:
//
//   * A data member wants plain `= value`, no braces: `cc::atomic<bool> _p{false}` -> `= false`.
//     Two things can go wrong, and neither is decidable without types. It can stop compiling, because an
//     explicit constructor refuses copy-init outright — a trade we accept deliberately, since an explicit
//     constructor behind a default member initializer is suspect anyway, and saying so on the right-hand
//     side is the answer (`cc::atomic<bool> _p = cc::atomic<bool>(false);`). It can also, for a type with
//     an `initializer_list` constructor, silently select a DIFFERENT constructor: `T v{1}` prefers the
//     init-list overload where `T v = 1` takes `T(int)`. That second one is why this cannot be a fix at
//     all, and why k_brace_drop_safe below is a whitelist of types confirmed free of both hazards rather
//     than a list of exceptions.
//
//     Narrowing is NOT among the hazards, though it looks like it should be: braces reject a narrowing
//     conversion and `=` permits one, so `int x{1.5}` is ill-formed while `int x = 1.5` truncates. But a
//     brace initializer that is in the tree already compiles, so no narrowing conversion is there to
//     un-reject.
//
//   * A non-member wants the `auto` form: `cc::random rng{u64(seed)}` -> `auto rng = cc::random(u64(seed));`.
//     This one is only a hint for a second, sharper reason: it rewrites `{…}` into `(…)`, and those are
//     not the same call. `std::vector<int>{1, 2}` is a two-element vector; `std::vector<int>(1, 2)` is one
//     element holding 2. Our own types are mostly free of that footgun — we consider it one — but `std::`
//     is not, so no automatic rewrite may make this substitution. It also has to carry the declaration's
//     specifiers across (`static`, `constexpr`, east-const, `alignas`), which is why the hint spells the
//     form out in prose rather than shipping edits for it.

/// Type spellings confirmed to take the braceless form: no explicit constructor, no `initializer_list`
/// overload to switch to. The hint states those without a caveat.
/// Grows as types are checked; being absent means "we have not verified", not "known bad".
constexpr cc::string_view k_brace_drop_safe[] = {"cc::atomic", "std::atomic"};

/// The declaration text ahead of the declarator-id — `static cc::atomic<int>` for `static cc::atomic<int> s{1}`.
///
/// Empty when that text cannot be a type. The case that matters is a multi-declarator statement: for `b` in
/// `int a{1}, b{2}` the text ahead of the declarator is `int a{1},`, an earlier declarator rather than a
/// type. A `{`, `}`, `=` or `;` in there is the tell — none of them appears in a type spelling, while a
/// comma does (`cc::array<int, 3>`) and so cannot be used for this.
cc::string_view type_text_of(lint_context const& ctx, node const& v)
{
    if (v.declarator.byte_begin <= v.span.byte_begin)
        return {};
    auto const head
        = source_span{.file_id = v.span.file_id, .byte_begin = v.span.byte_begin, .byte_end = v.declarator.byte_begin};
    auto const text = ctx.source.span_text(head);

    for (auto const c : text)
        if (c == '{' || c == '}' || c == '=' || c == ';')
            return {};

    isize end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return text.subview({.start = 0, .end = end});
}

/// Whether the declaration is a plain `T name`, with nothing the `auto` rewrite would have to carry across.
/// A specifier or attribute in front (`static`, `alignas(64)`, `[[…]]`) or a trailing cv-qualifier (east
/// const) both mean the hint stops naming a concrete type and describes the shape instead.
bool type_is_plain(cc::string_view type_text)
{
    if (type_text.empty())
        return false;
    for (auto const kw : {"static ", "constexpr ", "consteval ", "constinit ", "thread_local ", "inline ", "extern ",
                          "mutable ", "register ", "const ", "volatile ", "alignas", "[", "typename "})
        if (type_text.starts_with(cc::string_view(kw)))
            return false;
    return !type_text.ends_with(cc::string_view("const")) && !type_text.ends_with(cc::string_view("volatile"));
}

bool is_brace_drop_confirmed(cc::string_view type_text)
{
    for (auto const t : k_brace_drop_safe)
        if (type_text.starts_with(t))
            return true;
    return false;
}

/// Whether the braces around `inner` can be dropped without rewriting the initializer's meaning: exactly
/// one element, and not a designated initializer. `{}` cannot (`= ` is not valid), `{a, b}` cannot
/// (`= a, b` is a comma expression), `{.a = 1}` cannot (a designator needs its braces). Commas inside a
/// nested group are part of one element, so `{f(a, b)}` can.
///
/// Only `(`/`[`/`{` count as grouping. Tracking `<`/`>` too would misread the comparison in `{a > b}`,
/// and getting it wrong the other way — a comma inside template arguments — only costs a missing hint.
bool inner_is_single_value(cc::string_view inner)
{
    if (inner.empty() || inner[0] == '.')
        return false;

    isize depth = 0;
    for (auto const c : inner)
    {
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == ',' && depth == 0)
            return false;
    }
    return true;
}

/// The hint for a data member: drop the braces. Carries the edit, and says whether we have confirmed the
/// type takes it.
cc::optional<hint> member_hint(lint_context const& ctx, node const& v)
{
    auto const inner = ctx.source.span_text(v.init_inner);
    if (!inner_is_single_value(inner))
        return {};

    auto const type_text = type_text_of(ctx, v);
    auto const replacement = cc::string(" = ") + cc::string(inner);

    auto message = cc::string();
    if (is_brace_drop_confirmed(type_text))
        message = cc::string("a data member reads better without the braces — `") + type_text
                + "` is confirmed to take the braceless form";
    else
    {
        message = cc::string("a data member reads better without the braces — check it against an explicit "
                             "constructor (which refuses copy-init) and an `initializer_list` overload (which "
                             "copy-init would step past)");
        // Spell the escape hatch only when the declaration is a plain `T name`; otherwise the text ahead of
        // the declarator carries specifiers (`alignas(64) cc::atomic<int>`) that must not read as a type.
        if (type_is_plain(type_text))
            message += cc::string(", then name the type instead: `= ") + type_text + "(" + cc::string(inner) + ")`";
    }

    return hint{
        .message = cc::move(message),
        .edits = {text_edit{
            .span
            = {.file_id = v.declarator.file_id, .byte_begin = v.declarator.byte_end, .byte_end = v.init_span.byte_end},
            .replacement = replacement,
        }},
    };
}

/// The hint for a local or namespace-scope variable: the `auto` form, in prose only. Naming the type on
/// the right turns `{…}` into `(…)`, which is a different call for some `std::` types, and the
/// declaration's specifiers would have to move — neither is safe to spell as an edit.
cc::optional<hint> non_member_hint(lint_context const& ctx, node const& v)
{
    auto const inner = ctx.source.span_text(v.init_inner);
    if (inner.empty())
        return {}; // `auto v = T()` for a value-init is a rewrite with no reader benefit

    auto const name = ctx.source.span_text(v.name);
    auto const type_text = type_text_of(ctx, v);

    // Only name a concrete type when the declaration is a plain `T name` — otherwise describe the shape.
    if (!type_is_plain(type_text))
        return hint{.message = cc::string("a non-member prefers the `auto` form — `auto ") + name + " = <type>("
                             + cc::string(inner)
                             + ");` — keeping the declaration's specifiers and remembering that `(…)` is "
                               "not always the same call as `{…}`"};

    return hint{.message = cc::string("a non-member prefers the `auto` form — `auto ") + name + " = " + type_text + "("
                         + cc::string(inner) + ");` — check that `(…)` calls what `{…}` did"};
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
            .suggested_hint = v.scope == decl_scope::record_scope ? member_hint(ctx, v) : non_member_hint(ctx, v),
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
