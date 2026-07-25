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
* **`rules/`** — `rule` / `finding` / `fix` types, the single `registry`, the `engine`, and one file per rule.
* **`report/`** — the grouped-by-rule reporter.
* **`compdb/`** — reserved for the `compile_commands.json` reader (not built yet).

## Spans are the backbone

Every token and every syntax node carries a `source_span`.
Line/column are never stored — they are resolved lazily from the buffer, only when a finding is reported.
This is what makes accurate fix-its possible, and it is the foundation the macro model will build on.

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

The parser recognizes only what the rules need: namespaces, records (`class`/`struct`/`union` with a body), function bodies, nested blocks, lambda bodies — all descended — and the variable declarations inside them, with their initializer form.
Everything else is skipped as opaque.
It walks declaration-by-declaration with a prefix-aware segment scanner that tracks bracket depth by skipping balanced groups.

**The scope distinction is the whole point.** Every declaration node carries a `decl_scope` — `record_scope`, `namespace_scope`, or `function_scope` — so a rule never has to work out where it is. Only a real parse can do that, and only a real parse tells a declaration apart from a constructor's mem-initializer or an aggregate at a call site.

**One statement can declare several variables.** `int a{1}, b[2]{3}, c = 4, d;` is scanned declarator-by-declarator past each top-level `,`, re-running the brace-vs-`=`-vs-`;` decision each time, so it yields a node for `a` and `b` and nothing for `c` or `d`.
Each node carries both a `name` (the declarator-id, which is what a message says) and a `declarator` span reaching through any array suffix — `b[2]`, not `b`.
The two differ exactly where a rewrite would otherwise delete the bound, so a rule that replaces an initializer starts at `declarator.byte_end`, never at `name.byte_end`.

### The four judgements that keep function-scope parsing honest

Descending into function bodies is where false positives would come from, so each is decided explicitly:

* **Mem-initializer vs function body.** In `S() : a{1}, b{2} {}` every brace group looks alike. Only the *last* one is the body: a mem-initializer is always followed by `,` or by the body's own `{`, so the scanner keeps going while either follows and descends only into the group where neither does.
* **Nested block vs initializer.** At function scope a `{` with no declarator in front of it is a block (`{ … }`, an `else` / `do` / `try` body), not an initializer — it is descended, not run past.
* **Statement keyword.** `return`, `throw`, `case`, `co_return`, … at the top of a segment disqualify it from being a declaration, which is what stops `return P{1, 2};` from reading as one.
* **Enough tokens to be a declaration.** Outside a record body a brace init needs at least a type *and* a declarator ahead of it, so the temporary `T{1};` is not read as declaring `T`.

Lambda bodies are reached by a separate sweep: any group being skipped at function scope is walked for a `]` followed — past an optional parameter list, `mutable` / `noexcept` / a trailing return type — by `{`. That `]`-then-`(`-or-`{` shape is what separates a lambda introducer from a subscript `a[i]` and an attribute `[[nodiscard]]`. It is what reaches `auto f = [] { int y{0}; };` and `run([] { … });` alike.

### Known corner-cuts (each pinned by a corpus case)

Documented so the boundary does not regress silently:

* **Function-pointer data member with init** `void(*cb)(){…};` may mis-segment (the extra `()` reads as a parameter list).
* **`#if 0` disabled members** are still parsed as live code (directives are opaque) — a possible false positive, resolved only at the future preprocessor milestone.
* **Deeply nested statements are reached, but blocks inside a skipped group are not** — a body only becomes visible through the paths above, so an exotic construct can still hide one.

## Relationship to the clang-tidy gates

shaped-linter is the sibling of the [clang-tidy gate framework](../../lint/) — the place for rules clang-tidy structurally cannot express (e.g. macro-placement).
It shares one philosophy: **every rule carries a mandatory rationale**, and output is a grouped-by-rule digest that leads each group with that `why`.
It runs as `dev.py lint shaped`, and is a `dev.py check` gate (`shaped-lint`) that runs **dirty-only** — like the clang-tidy gates, so the rules adopt incrementally rather than requiring a repo-wide sweep first.

See [writing-a-rule.md](writing-a-rule.md) to add a rule, and [coding-guidelines.md](coding-guidelines.md) for the conventions it follows — notably the two-layer test split (smoke tests plus a markdown corpus).
