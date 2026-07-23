# Writing a shaped-linter rule

This is the step-by-step for adding a rule to shaped-linter.
It assumes you have skimmed the [readme](../readme.md) and the [clean-core cheat sheet](../../../libs/base/clean-core/cheat-sheet.md).

A rule is a small, stateless value: a stable id, a mandatory rationale, the highest pipeline layer it needs, and a `check` function that walks that layer and emits findings.
The engine only builds the expensive layers a rule actually asks for.

## The pipeline, and which layer to pick

```
source_buffer ─▶ lexer ─▶ token_stream ─▶ parser ─▶ syntax_tree ─▶ rule engine ─▶ findings ─▶ reporter
```

Pick the **lowest** layer that can express your rule — it is cheaper and simpler:

* `rule_layer::tokens` — the rule reads the `token_stream` directly (spelling-level checks: a banned identifier, a macro name, a literal shape).
  The parse tree is not built if no enabled rule needs it.
* `rule_layer::syntax_tree` — the rule walks the parsed `syntax_tree` (structural checks: something about a *member* vs a local, a record's shape).
  This is what [`member-default-init-assignment`](../src/shaped-linter/rules/member_default_init_assignment.cc) uses.

A structural rule must use the tree, not a token scan — the tree is what tells a member initializer apart from a local, a base-class init, or an aggregate at a call site.

## The slug and the rationale are mandatory

Every rule carries:

* an `id` — a stable, greppable kebab-case slug, like a clang-tidy check name (`member-default-init-assignment`).
  It is printed in brackets on every finding line (`… [member-default-init-assignment]`), so it is easy to grep and to silence.
* a `rationale` — one sentence on *why*, ideally with the preferred fix.
  The reporter leads every group with it, and `all_rules()` asserts it is non-empty.
  This mirrors the clang-tidy gate culture, where every gate carries its `why`.

## Steps

### 1. Add the rule source

Create `src/shaped-linter/rules/<your-rule>.hh` / `.cc`.
Expose one accessor returning the rule by reference:

```cpp
// <your-rule>.hh
#pragma once
#include <shaped-linter/rules/rule.hh>
namespace scl
{
rule const& your_rule();
} // namespace scl
```

In the `.cc`, put the id, the rationale, and the `check` in an anonymous namespace, then hand them to a function-local static `rule`:

```cpp
namespace
{
constexpr cc::string_view k_id = "your-rule";
constexpr cc::string_view k_rationale = "why this matters, and the preferred fix.";

void check(lint_context& ctx)
{
    // ctx.source  — the source_buffer (span_text, line_text)
    // ctx.tokens  — the token_stream (for token-layer work)
    // ctx.tree    — the syntax_tree (empty unless some enabled rule needs it)
    // ctx.report({...}) — emit a finding
}
} // namespace

rule const& your_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::syntax_tree, // or rule_layer::tokens
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
```

### 2. Emit findings (and an optional fix)

A `finding` carries the `rule_id`, the `span` to underline, a `message`, a `severity`, and an optional `fix`.
A `fix` is one or more `text_edit`s — each replaces a byte range with new text.
Wire a fix in whenever you know the rewrite; the reporter shows it and `--fix` applies it (back-to-front per file, so offsets stay valid).

```cpp
ctx.report({
    .rule_id = k_id,
    .span = the_span_to_underline,
    .message = cc::string("what is wrong"),
    .sev = severity::warning,
    .suggested_fix = fix{.edits = {text_edit{.span = range_to_replace, .replacement = cc::string("new text")}}},
});
```

Spans are `{file_id, byte_begin, byte_end}` (half-open).
Get text with `ctx.source.span_text(span)`; resolve to line/column happens later, in the reporter.

Output is UTF-8: `main` sets the Windows console to `CP_UTF8` at startup, so the repo's typography (em dashes, `…`) and any UTF-8 in the echoed source line render correctly.
Still, prefer a short ASCII `message` (e.g. `= value` over `= …`) — it stays clean when grepped from a log.

### 3. Register it

Add one line to [`registry.cc`](../src/shaped-linter/rules/registry.cc)'s `all_rules()`:

```cpp
v.push_back(your_rule());
```

The registry is the single list of rules — mirroring how the clang-tidy gate config is one list of gates.

### 4. Add it to the build

List the new `.cc` in [`CMakeLists.txt`](../CMakeLists.txt) under `shaped-linter-core`, and its test under `shaped-linter-test`.

### 5. Test it

Two layers, both nexus:

* **Raw `TEST`s** for units and hand-written cases — use `run_rules_on_text("<snippet>")` and assert on the findings (count, message, and the fix replacement).
  The whole detect-and-fix path is `apply_edits(src, edits)` (see [`engine-test.cc`](../tests/rules/engine-test.cc)).
* **A data-driven corpus** via `INVOCABLE_TEST` + a driver `TEST` that calls `nx::invoke_tests(case.name, case)` per case (see [`member_default_init_assignment-test.cc`](../tests/rules/member_default_init_assignment-test.cc)).
  Use a unique case struct as the key, and wire the driver so the invocable is never an orphan.

**Always add both a positive and a negative** — a case that must fire and a look-alike that must not.
For a structural rule, the negatives are the point: prove it does *not* fire on the local, the base-class init, the call-site aggregate.
When you cut a corner in the parser, pin it with a test (even one asserting the known-wrong-but-safe behavior), so the boundary is documented and does not regress silently.

## Growing the lexer or parser

A new rule often needs the parser to recognize a construct it currently skips.
Grow it rule-by-rule — add exactly what the rule needs, keep the rest opaque, and lock the new shape with a `parser-test.cc` case.
The parser deliberately handles only what the rules use; that is a feature, not a gap to fill speculatively.

Real-world inputs are the best tests: when a rule misfires on an actual repo file, reduce it to the smallest snippet that reproduces and add that snippet as a regression test (that is how the "directives before a namespace" and "static_assert before members" parser tests were born).
