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
  This is what [`default-init-assignment`](../rules/cpp-style/default-init-assignment/default_init_assignment.cc) uses.

A structural rule must use the tree, not a token scan — the tree is what tells a declaration apart from a constructor's mem-initializer or an aggregate at a call site, and it is what carries `node::scope` (`record_scope` / `namespace_scope` / `function_scope`).
Read the scope off the node; never re-derive it in a rule.

## The slug and the rationale are mandatory

Every rule carries:

* an `id` — a stable, greppable kebab-case slug, like a clang-tidy check name (`default-init-assignment`).
  It opens every finding (`[default-init-assignment] the message`), so it is easy to grep and to silence.
* a `rationale` — one sentence on *why*, ideally with the preferred fix.
  It prints once per run, in the `rule rationale` section under the findings, and `all_rules()` asserts it is non-empty.
  This mirrors the clang-tidy gate culture, where every gate carries its `why`.

## Steps

### 1. Create the rule's folder

**A rule is a folder**, holding everything about it: `rules/<group>/<your-rule>/`.

```text
rules/cpp-style/default-init-assignment/
    default_init_assignment.hh      the description and main documentation
    default_init_assignment.cc      the implementation
    default_init_assignment-test.cc the smoke tests (more than one is fine)
    default_init_assignment.md      the corpus
```

The folder is named **exactly** for the rule id, so a slug read off a finding is the path to everything
about it; the files inside are snake_case as everywhere else. Pick the group by what the rule is about, and
add a new one (a directory with its own `CMakeLists.txt`) when none fits.

**The header carries the rule's documentation** — what it enforces, what it deliberately leaves alone, and
which layer it walks. That doc comment is what a reader meets first; the `.cc` explains only how.

```cpp
// <your-rule>.hh
#pragma once
#include <shaped-linter/rules/rule.hh>
namespace scl
{
/// The `your-rule` rule.
/// What it enforces, in the first line. Then the boundary: what it deliberately stays quiet on, and why.
rule const& your_rule();
} // namespace scl
```

In the `.cc` — which includes its own header with quotes — put the id, the rationale, and the `check` in an anonymous namespace, then hand them to a function-local static `rule`:

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

### 2. Emit findings (and an optional fix or hint)

A `finding` carries the `rule_id`, the `span` to underline, a `message`, a `severity`, and — independently — an optional `fix` and an optional `hint`.
Both are one or more `text_edit`s, each replacing a byte range with new text.

```cpp
ctx.report({
    .rule_id = k_id,
    .span = the_span_to_underline,
    .message = cc::string("what is wrong"),
    .sev = severity::warning,
    .suggested_fix = fix{.edits = {text_edit{.span = range_to_replace, .replacement = cc::string("new text")}}},
});
```

#### `fix` vs `hint` — the line between them

**A `fix` must be safe to apply unattended.** Wherever the rule fires, applying it compiles and preserves behavior.
The reporter shows it and `--fix` applies it (back-to-front per file, so offsets stay valid).
Hold a rewrite to that bar or it does not belong in a fix: `--fix` runs across the whole tree at once, so one bad rewrite is a tree-wide breakage.

**A `hint` is the better form that only a human can sign off on**, and `--fix` never applies it.
Reach for it when the nicer rewrite may fail to compile, or — the serious case — may silently change what the code means.
A hint carries a `message` saying what to weigh, plus optional `edits`; a hint whose better form cannot be spelled mechanically carries prose alone.

```cpp
    .suggested_hint = hint{
        .message = cc::string("why a human has to decide, and what the better form is"),
        .edits = {text_edit{.span = range_to_replace, .replacement = cc::string("nicer text")}},
    },
```

The two are independent — a finding may carry both, and then the fix is what lands while the hint is printed alongside.
[`default_init_assignment.cc`](../rules/cpp-style/default-init-assignment/default_init_assignment.cc) is the worked example: its fix moves the `=` in and keeps the braces (always safe), while its hint offers the braceless `= value` for a member and the `auto v = T(value)` form for a local.
Its block comment spells out which hazard rules out which rewrite — worth reading before you decide where your own rewrite belongs.

Nothing in the engine reads a hint's edits; `apply_fixes` looks only at `suggested_fix`, and [`engine-test.cc`](../tests/rules/engine-test.cc) pins that.

#### A fix that also has to add a line

Some rewrites only compile once the file gains something shared — an include, a using-directive.
Because "safe to apply unattended" is a promise about each fix *on its own*, the shared edit rides on **every** finding, not on the first one only.
An **empty span** is the insertion: nothing is removed and `replacement` is spliced in at that offset.

```cpp
.suggested_fix = fix{.edits = {
    text_edit{.span = range_to_replace, .replacement = cc::string("new text")},
    text_edit{.span = {.file_id = fid, .byte_begin = off, .byte_end = off}, .replacement = cc::string("...\n")},
}},
```

`collect_fix_edits` merges byte-identical edits, so N findings asking for the same line still splice it exactly once — which is why the insertion must be computed identically for every finding in the file, not relative to the one being reported.
[`qualified_primitive.cc`](../rules/cpp-style/qualified-primitive/qualified_primitive.cc) is the worked example; its `using_directive_insertion` also shows the other half of the job — deciding whether a safe offset exists at all.

Spans are `{file_id, byte_begin, byte_end}` (half-open).
Get text with `ctx.source.span_text(span)`; resolve to line/column happens later, in the reporter.

Output is UTF-8: `main` sets the Windows console to `CP_UTF8` at startup, so the repo's typography (em dashes, `…`) and any UTF-8 in the echoed source line render correctly.
Still, prefer a short ASCII `message` (e.g. `= value` over `= …`) — it stays clean when grepped from a log, and through a mirror whose encoding you do not control.

#### What you do NOT write

The rendering is entirely framework-side, so the fields above are the whole of a rule's output work:

```
[default-init-assignment] member default initializer should use assignment form (`= value`), not brace form
  --> libs/base/clean-core/src/clean-core/thread/atomic.hh:10:27
   |
 8 | struct worker
 9 | {
10 |     cc::atomic<bool> _pending{false};
   |                              ^^^^^^^
11 |     int _retries = 3;
   |
  fix: replace `{false}` with `= {false}` (applied by --fix)
  help: a data member reads better without the braces
        consider `= false` (not applied)
```

The header, the location line, the excerpt with its context, the carets under exactly your `span`, the `fix:` / `help:` lines and the rationale section all come from the finding.
Never format any of it into the `message`.

#### Optional: labels, for a finding that is a relation

Most findings point at one place and need nothing more.
When a finding is only intelligible as a relation between two places, `finding` takes labels:

```cpp
    .primary_label = cc::string("declared with braces here"),
    .secondary = {{.span = other_span, .text = cc::string("and used here")}},
```

The primary label prints next to the `^^^`; secondary spans are underlined with `---`.
Two labels on one line become a ladder, and a secondary span in another file gets its own `::: path` block.
All of that is [`report/snippet.cc`](../src/shaped-linter/report/snippet.cc)'s job.

### 3. Register it

Add one line to [`registry.cc`](../src/shaped-linter/rules/registry.cc)'s `all_rules()`, and the include for your header:

```cpp
#include <rules/<group>/<your-rule>/<your_rule>.hh>
...
v.push_back(your_rule());
```

The registry is the single list of rules — mirroring how the clang-tidy gate config is one list of gates.
It is the one place outside your folder that names your rule.

### 4. Add it to the build

Name the `.cc` and each `-test.cc` in your group's `CMakeLists.txt` — see [`rules/cpp-style/CMakeLists.txt`](../rules/cpp-style/CMakeLists.txt):

```cmake
target_sources(shaped-linter-core PRIVATE
    <your-rule>/<your_rule>.cc
)
if(SC_BUILD_TESTS)
    target_sources(shaped-linter-test PRIVATE
        <your-rule>/<your_rule>-test.cc
    )
endif()
```

A whole new group also needs its `add_subdirectory(rules/<group>)` at the bottom of the [root `CMakeLists.txt`](../CMakeLists.txt).
A corpus file needs no CMake change at all — `rules/` is scanned recursively at run time.

### 5. Test it

Two layers, both nexus. The split and the corpus format are specified in [coding-guidelines.md](coding-guidelines.md); the short version:

* **Smoke tests** in `<your-rule>-test.cc`, in your rule's folder — ordinary `TEST` + `SECTION` with `run_rules_on_text("<snippet>")`, asserting on the findings (count, rule id, fix replacement).
  This is the scratchpad you build the rule in and where a regression gets pinned; keep it under ~200 lines.
  The whole detect-and-fix path is `apply_edits(src, edits)` (see [`engine-test.cc`](../tests/rules/engine-test.cc)) — that is the layer that pins what the rewritten source *looks like*, which the corpus never does.
* **A markdown corpus** at `<your-rule>.md`, in the same folder — ordinary prose with annotated `cpp` blocks (see [`default_init_assignment.md`](../rules/cpp-style/default-init-assignment/default_init_assignment.md)).
  This is where breadth lives; adding a case is adding a fenced block, not writing C++.

The corpus annotations, in short — the full specification is in [coding-guidelines.md](coding-guidelines.md):

```text
```cpp [your-rule] fix=" = {0}"        one finding, and that is the rewrite it offers
```cpp [your-rule] [your-rule]         two findings; their fixes are not pinned
```cpp [your-rule] [your-rule] fix=" = 1" fix=" = 2"    two findings offering exactly these two rewrites
```cpp [your-rule] fix=" = {0}" hint=" = 0"             the same block, pinning both channels
```cpp ~[your-rule]                    must stay quiet
```cpp [your-rule] path="x.cc"         linted AS that file name, for a rule that reads it
```

Inside a quoted value `\n` / `\t` / `\r` are the real characters, so a fix that splices in a whole line is still spellable on the single line a fence gets.

Two things are easy to get wrong:

* **`fix=` binds to the rule annotation in front of it**, and a rule's fixes are pinned as a **set** — every replacement it produced, over every finding and every edit, duplicates merged.
  So naming one fix for a rule means naming them all, and order never matters.
* **The finding count comes from the `[rule-id]` annotations alone.** A `fix=` never adds one.

`hint=` is the same pin over the same rule's hint edits, tracked separately — a block may name only its fixes, only its hints, or both.
A prose-only hint contributes no edit, so its wording is pinned by the smoke test rather than the corpus, and so is the *absence* of a hint.

`path=` describes the **block**, not a rule, so it stands on its own and may appear anywhere in the info string — at most once.
Only the name is used, never the contents: a rule that tells a header from a translation unit sees the extension and nothing else.

A block is linted with `all_rules()` and the total must match exactly, so a second rule firing on your case fails until the block names it too — that is deliberate, it surfaces cross-talk.

**Always add both a positive and a negative** — a case that must fire and a look-alike that must not.
For a structural rule, the negatives are the point: prove it does *not* fire on the mem-initializer, the braced return, the call-site aggregate.
When you cut a corner in the parser, pin it in the corpus (even asserting the known-wrong-but-safe behavior), so the boundary is documented and does not regress silently.

## Growing the lexer or parser

A new rule often needs the parser to recognize a construct it currently skips.
Grow it rule-by-rule — add exactly what the rule needs, keep the rest opaque, and lock the new shape with a `parser-test.cc` case.
The parser deliberately handles only what the rules use; that is a feature, not a gap to fill speculatively.

Real-world inputs are the best tests: when a rule misfires on an actual repo file, reduce it to the smallest snippet that reproduces and add that snippet as a regression test (that is how the "directives before a namespace" and "static_assert before members" parser tests were born).
