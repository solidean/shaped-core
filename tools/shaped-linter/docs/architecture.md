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

The parser recognizes only what the rules need: namespaces (descended), records (`class`/`struct`/`union` with a body), and — inside a record body — data-member declarations with their initializer form.
Everything else is skipped as opaque.
It walks declaration-by-declaration with a prefix-aware segment scanner that tracks bracket depth by skipping balanced groups.

The scope distinction is the whole point: only a real parse tells a **member** initializer apart from a function-local, a namespace-scope global, a constructor init-list, or an aggregate at a call site.

### Known corner-cuts (each safe, each pinned by a test)

These err toward a miss (never a false positive), and are documented so the boundary does not regress silently:

* **Multi-declarator brace-init** `int a{1}, b{2};` records only the first.
* **Array data member** `T a[N]{…};` is skipped (the token before `{` is `]`, not a declarator-id).
* **Function-pointer data member with init** `void(*cb)(){…};` may mis-segment (the extra `()` reads as a parameter list).
* **`#if 0` disabled members** are still parsed as live code (directives are opaque) — a possible false positive, resolved only at the future preprocessor milestone.

## Relationship to the clang-tidy gates

shaped-linter is the sibling of the [clang-tidy gate framework](../../lint/) — the place for rules clang-tidy structurally cannot express (e.g. macro-placement).
It shares one philosophy: **every rule carries a mandatory rationale**, and output is a grouped-by-rule digest that leads each group with that `why`.
It runs as `dev.py lint shaped`, and is a `dev.py check` gate (`shaped-lint`) that runs **dirty-only** — like the clang-tidy gates, so the rules adopt incrementally rather than requiring a repo-wide sweep first.

See [writing-a-rule.md](writing-a-rule.md) to add a rule.
