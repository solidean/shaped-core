# shaped-linter architecture

A self-contained C++ linter: its own lexer and parser, no LLVM / libclang, built on clean-core.
Namespace `scl` (internals `scl::impl`).

## The layered pipeline

```
source_buffer ─▶ lexer ─▶ token_stream ─▶ parser ─▶ syntax_tree ─▶ rule engine ─▶ findings ─▶ reporter
```

Each rule declares the highest layer it needs (`tokens` or `syntax_tree`).
The engine builds the parse tree only when some enabled rule asks for it — cheap rules stay cheap.

* **`lex/`** — `source_buffer` (owns bytes + a line index), `source_span` (`{file_id, byte_begin, byte_end}`), `source_manager` (owns all buffers, resolves spans), `token` / `token_stream`, and the `lexer`.
* **`parse/`** — `syntax_tree` (an arena of `node`s referenced by id) and the recursive-descent `parser`.
* **`rules/`** (under `src/`) — the framework only: `rule` / `finding` / `fix` types, the single `registry`, and the `engine`.
* **`report/`** — the diagnostic renderer: `snippet` (the line-numbered source view with its carets), `renderer` (a finding, and a whole run, as text), `style` (the presentation knobs), and `reporter` (the write to stdout).
* **`compdb/`** — reserved for the `compile_commands.json` reader (not built yet).

Concrete rules live **outside `src/`**, one folder each under `rules/<group>/<rule>/`, holding the rule's
header, implementation, smoke tests and corpus together.
`src/` is the framework a rule stands on; `rules/` is the rules themselves, and the group's `CMakeLists.txt`
names their files. The tool directory is a second include root, so a rule header is reached as
`<rules/cpp-style/default-init-assignment/default_init_assignment.hh>` — spelled only by
`registry.cc` and the rule's own files.

## Spans are the backbone

Every token and every syntax node carries a `source_span`.
Line/column are never stored — they are resolved lazily from the buffer, only when a finding is reported.
This is what makes accurate fix-its possible, and it is the foundation the macro model will build on.

The renderer is the one place that resolves them, which is why `source_buffer` also indexes by line (`line_count`, `line_span`, `line_text`) on top of the byte-offset lookups.

## Rendering is framework work, not rule work

A rule reports a span, a message and optionally a fix or a hint.
Everything a reader sees — the `[rule-id] message` header, the `--> path:line:col` line, the numbered excerpt, the carets under the exact span, the `fix:` / `help:` lines, the rationale section, the summary — is produced by `report/` from those fields alone.
A rule that wants to point at a second place adds a `label` to `finding::secondary`.
The layout of two underlines on one line, of a span that runs over several lines, and of a label in another file is the renderer's problem, not the rule's.

`render_snippet` and `render_report` are pure functions returning `cc::string`, and `report_style::color` is passed in rather than read from the process-global `cc::console` state.
That is what makes the exact output testable (`tests/report/`), and keeps `--fix` free to rewrite the files afterwards.

## Macro provenance is reserved, not implemented

The single most important "anticipate the real thing" decision: the data model already supports the eventual answer to *"was this token produced by a macro, and which invocation?"*, even though v1 does not expand macros.

* `token::expansion_id` is a reserved hook. `0` means "spelled directly in source" (one token, one contiguous range).
  A future `impl::expansion_table` will map non-zero ids to `{invocation_span, definition_span}`; `token::span` always stays the **spelling** location.
* No code may assume "one token ⇔ one contiguous source range" beyond reading `.span`.

v1 treats `#…` directives as opaque tokens and does not expand them.
The parser skips directive tokens entirely (they stay in the token stream for future macro-placement rules).

## What the lexer gets right, and what it cuts

Handled: identifiers/keywords, integer/float literals with digit separators and suffixes, char literals, strings including raw `R"d(…)d"` and all encoding prefixes, `//` and `/* */` comments, line continuation, maximal-munch punctuators (`<=>`, `::`, `>>`, …), and `#…` directives as one opaque token.
`<` / `>` / `>>` are emitted raw; the parser resolves angle nesting (splitting `>>` into two closers).

## What the parser recognizes, and what it skips

The parser recognizes only what the rules need: namespaces, records (`class`/`struct`/`union` with a body), function bodies, nested blocks, lambda bodies, control-flow statements — all descended — and the variable declarations inside them, with their initializer form.
Everything else is skipped as opaque.
It walks declaration-by-declaration with a prefix-aware segment scanner that tracks bracket depth by skipping balanced groups.

**Namespaces and using-directives are nodes**, because *which names are in scope here* is a scope question and rules own no scope logic.
A `namespace_definition` carries its name as written (`cc::impl`) and its `body` span; a `using_directive` carries the namespace it nominates and an `effect` span — the bytes from past its `;` to the end of the enclosing scope, which is exactly where it is in force.
Both are answered by testing a byte offset against a span, so a rule never walks parents.
A namespace *alias*, a using-declaration and a type alias nominate nothing and produce no node.

**The scope distinction is the whole point.** Every declaration node carries a `decl_scope` — `record_scope`, `namespace_scope`, or `function_scope` — so a rule never has to work out where it is. Only a real parse can do that, and only a real parse tells a declaration apart from a constructor's mem-initializer or an aggregate at a call site.

**One statement can declare several variables.** `int a{1}, b[2]{3}, c = 4, d;` is scanned declarator-by-declarator past each top-level `,`, re-running the brace-vs-`=`-vs-`;` decision each time, so it yields a node for `a` and `b` and nothing for `c` or `d`.
Each node carries both a `name` (the declarator-id, which is what a message says) and a `declarator` span reaching through any array suffix — `b[2]`, not `b`.
The two differ exactly where a rewrite would otherwise delete the bound, so a rule that replaces an initializer starts at `declarator.byte_end`, never at `name.byte_end`.

### The four judgements that keep function-scope parsing honest

Descending into function bodies is where false positives would come from, so each is decided explicitly:

* **Mem-initializer vs function body.** In `S() : a{1}, b{2} {}` every brace group looks alike. Only the *last* one is the body: a mem-initializer is always followed by `,` or by the body's own `{`, so the scanner keeps going while either follows and descends only into the group where neither does.
* **Nested block vs initializer.** At function scope a `{` with no declarator in front of it is a block (`{ … }`, a `try` / `catch` body), not an initializer — it is descended, not run past.
* **Statement keyword.** `return`, `throw`, `case`, `co_return`, … at the top of a segment disqualify it from being a declaration, which is what stops `return P{1, 2};` from reading as one.
* **A type ahead of the declarator-id.** A brace init needs a type *and* a declarator, and the declarator-id's `::`-joined name run is what decides whether one is left over.
  In `cc::atomic<int> x{0}` the run at `x` is just `x`, with the type ahead of it; in `cc::void_function{}()` the run is the whole segment, so this is a temporary being called, not a declaration of `void_function`.
  That also settles `T{1};`, `cc::T{1};` and `cc::vector<int>{1, 2};` — while `int S::x{0};`, an out-of-line static member definition, stays a declaration because its run starts after the `int`.
  Counting the tokens ahead of the brace cannot tell these apart: a qualified name has several and a type none of them.

Lambda bodies are reached by a separate sweep: any group being skipped at function scope is walked for a `]` followed — past an optional parameter list, `mutable` / `noexcept` / a trailing return type — by `{`. That `]`-then-`(`-or-`{` shape is what separates a lambda introducer from a subscript `a[i]` and an attribute `[[nodiscard]]`. It is what reaches `auto f = [] { int y{0}; };` and `run([] { … });` alike.

### Statement forms, parsed as forms rather than inferred

The segment scanner above is a *declaration* scanner. A statement whose shape the grammar fixes is parsed as that shape instead, so its parts land in the right place by construction — the same treatment `namespace` already gets.

* **`if` / `switch` / `while` / `for`** — a parenthesized header, then a body.
  The header is a declaration scope: `for (int i{0}; …)` and `if (auto x{g()}; x > 0)` each declare a local, and the header's own `begin` is the first token inside the parens, so the type a rule reconstructs is `int`, not `for (int`.
  Its clauses are read as statements, which is why a `for`'s second and third clause and any plain condition come out as expressions.
  `if constexpr` / `if consteval` are the same form with a specifier between the keyword and the header.
* **A range-for splits at its `:`.** Only the range-declaration ahead of it is a scope; the range behind it is an expression, so the `{…}` in `for (auto p : {"a", "b"})` initializes the range and belongs to no declarator. A conditional's `:` is paired off against its `?`, which keeps `for (int i{0}; c ? a : b; ++i)` a plain three-clause `for`.
* **`else` and `do`** — a body with no header of its own; `do`'s trailing `while ( … )` holds an *expression* by the grammar, so it is only swept for lambda bodies, never parsed as a scope.
* **A body is exactly one statement**, braced or not. `if (c) int y{0};` declares a local as surely as `if (c) { int y{0}; }` does, and — the other direction — the statement is over at that point, so `if (c) g(a, T{1});` is an ordinary call again.

Before this, control flow worked by accident: its paren group set the "saw a parameter list" flag, which made the following `{` look like a function body. That reached braced bodies and nothing else — headers were invisible and a braceless body silently dropped its declaration.

`try` / `catch` are deliberately still on the generic path: the grammar requires their bodies to be compound statements, so the nested-block judgement already places them correctly.

### Known corner-cuts (each pinned by a corpus case)

Documented so the boundary does not regress silently:

* **Function-pointer data member with init** `void(*cb)(){…};` may mis-segment (the extra `()` reads as a parameter list).
* **`#if 0` disabled members** are still parsed as live code (directives are opaque) — a possible false positive, resolved only at the future preprocessor milestone.
* **Deeply nested statements are reached, but blocks inside a skipped group are not** — a body only becomes visible through the paths above, so an exotic construct can still hide one.
* **A binary `&&` reads as declarator punctuation.** `ok && n < T{1}` reports `T`, because `&&` is also an rvalue reference and so does not break the name run that decides whether a type is left over. Separating the two needs a notion of declarator position, which the parser does not have yet — the natural next increment.
* **A structured binding is invisible.** `for (auto [a, b] : m)` declares nothing the parser sees, since the `[…]` is skipped as a balanced group. Safe (invisible, not misread) but incomplete.

## Relationship to the clang-tidy gates

shaped-linter is the sibling of the [clang-tidy gate framework](../../lint/) — the place for rules clang-tidy structurally cannot express (e.g. macro-placement).
It shares one philosophy: **every rule carries a mandatory rationale**, printed once per run in the `rule rationale` section under the findings.
It runs as `dev.py lint shaped`, and is a `dev.py check` gate (`shaped-lint`) that runs **dirty-only** — like the clang-tidy gates, so the rules adopt incrementally rather than requiring a repo-wide sweep first.

See [writing-a-rule.md](writing-a-rule.md) to add a rule, and [coding-guidelines.md](coding-guidelines.md) for the conventions it follows — notably the two-layer test split (smoke tests plus a markdown corpus).
