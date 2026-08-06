# shaped-linter architecture

A self-contained linter: its own lexers and parser, no LLVM / libclang, built on clean-core.
Namespace `scl` (internals `scl::impl`).

## The layered pipeline

The [readme](../readme.md#how-it-works) draws it.
A `source_buffer` is lexed into a `token_stream`, which feeds the C++ parser on one side and prose extraction on the other; the engine reads whichever of those the rules asked for.

Each rule declares the layer it needs (`tokens`, `syntax_tree` or `prose`) and the **languages** it applies to.
The engine builds only what some enabled rule asked for — cheap rules stay cheap, and a rule never sees a file it did not ask for.

## Three languages, one dispatch point

A file's extension picks its front end, in `run_rules` and nowhere else: `.cc` / `.hh` are C++, `.py` is Python, `.md` is markdown, anything unrecognized is C++.
Below that call every layer is already language-correct, so a rule reads what it declared without ever asking where it is.

* **C++** — the full pipeline: lexer, then a recursive-descent parser producing a `syntax_tree`.
* **Python** — `lex_python` produces the *same* `token_stream`, so spans, the renderer and `--fix` work unchanged.
  It lexes and nothing more, except that it tracks indentation as zero-width `indent` / `dedent` tokens — enough to tell a nested block from a top-level one without a grammar.
  There is no Python parser and no plan for one until a rule needs it.
* **Markdown** — `scan_markdown` tags each line prose / blank / fence / code / table / frontmatter, and that is the whole front end.
  Deliberately not babel::markdown: babel gives a block's starting line but no byte offsets, and `shaped-linter-core` stays on clean-core alone.
  babel is the corpus reader in the *test* binary only — see [coding-guidelines](coding-guidelines.md#how-the-corpus-reaches-the-test-binary).

`rule::languages` is a bitmask over `source_language`, defaulting to C++ alone — so the existing C++ rules are structurally safe from ever meeting a markdown file.

## Prose is a layer, not a step

`prose_view` is the file's comments and body text, extracted per language into blocks of lines with the markers stripped.
That is C++ `//` and `/* */`, Python `#` and docstrings, and markdown body text outside fenced code.
It is a branch off the token stream rather than a deeper layer, and it is the one layer that exists for all three languages.

Lines carry their exact `source_span`, so a prose finding's carets land on the offending words.
They group into **blocks** — adjacent line comments, one block comment, one paragraph.
That is because several prose judgements are block-level: whether a line is a short orphan of the one above it cannot be answered line by line.

* **`lex/`** — `source_buffer` (owns bytes + a line index), `source_span` (`{file_id, byte_begin, byte_end}`), `source_manager` (owns all buffers, resolves spans).
  Plus `source_language` (the dispatch key), `token` / `token_stream`, the C++ `lexer`, the `python_lexer`, and the `markdown_scanner`.
* **`parse/`** — `syntax_tree` (an arena of `node`s referenced by id) and the recursive-descent `parser`. C++ only.
* **`prose/`** — `prose_view` and the per-language extraction that fills it, plus the `prose apply` plan parser and applier.
* **`rules/`** (under `src/`) — the framework only: `rule` / `finding` / `fix` types, the single `registry`, and the `engine`.
* **`report/`** — the diagnostic renderer: `snippet` (the line-numbered source view with its carets), `renderer` (a finding, and a whole run, as text).
  Plus `style` (the presentation knobs) and `reporter` (the write to stdout).
* **`cli/`** — `options` (the parsed command line and the usage text) and `changed_lines` (the `--changed-lines` spec `--dirty-only` passes in).

`src/` is the framework a rule stands on; the rules themselves live outside it, one folder each — see [coding-guidelines](coding-guidelines.md#a-rule-is-a-folder).
The one thing that is architectural rather than convention: the tool directory is a second include root.
A rule header is therefore reached as `<rules/cpp-style/default-init-assignment/default_init_assignment.hh>`, spelled only by `registry.cc` and the rule's own files.

## Spans are the backbone

Every token and every syntax node carries a `source_span`.
Line/column are never stored — they are resolved lazily from the buffer, only when a finding is reported.
This is what makes accurate fix-its possible, and it is what the reserved macro model below would build on.

The renderer is the one place that resolves them, which is why `source_buffer` also indexes by line (`line_count`, `line_span`, `line_text`) on top of the byte-offset lookups.

## Rendering is framework work, not rule work

A rule reports a span, a message and optionally a fix or a hint.
Everything a reader sees is produced by `report/` from those fields alone.
The `[rule-id] message` header, the `--> path:line:col` line, the numbered excerpt, the carets under the exact span.
Then the `fix:` / `help:` lines, the rationale section and the summary.
A rule that wants to point at a second place adds a `label` to `finding::secondary`.
The layout of two underlines on one line, of a span that runs over several lines, and of a label in another file is the renderer's problem, not the rule's.

`render_snippet` and `render_report` are pure functions returning `cc::string`, and `report_style::color` is passed in rather than read from the process-global `cc::console` state.
That is what makes the exact output testable (`tests/report/`), and keeps `--fix` free to rewrite the files afterwards.

## Macro provenance is reserved, not implemented

The single most important "anticipate the real thing" decision.
The data model already supports the eventual answer to *"was this token produced by a macro, and which invocation?"*, even though v1 does not expand macros.

* `token::expansion_id` is a reserved hook.
  `0` means "spelled directly in source" (one token, one contiguous range).
  A future `impl::expansion_table` will map non-zero ids to `{invocation_span, definition_span}`; `token::span` always stays the **spelling** location.
* No code may assume "one token ⇔ one contiguous source range" beyond reading `.span`.

v1 treats `#…` directives as opaque tokens and does not expand them.
The parser skips directive tokens entirely (they stay in the token stream for future macro-placement rules).

## What the lexer gets right, and what it cuts

Handled: identifiers and keywords, integer/float literals with digit separators and suffixes, char literals.
Strings including raw `R"d(…)d"` and all encoding prefixes; `//` and `/* */` comments; line continuation.
Maximal-munch punctuators (`<=>`, `::`, `>>`, …), and `#…` directives as one opaque token.
`<` / `>` / `>>` are emitted raw; the parser resolves angle nesting (splitting `>>` into two closers).

## What the parser recognizes, and what it skips

The parser recognizes only what the rules need, and skips everything else as opaque.
[parser.hh](../src/shaped-linter/parse/parser.hh) states the recognized set.
[parser.cc](../src/shaped-linter/parse/parser.cc) documents each judgement at the code that makes it.
What follows is the design claim behind that code, not a second copy of it.

**The scope distinction is the whole point.**
Every declaration node carries a `decl_scope` — `record_scope`, `namespace_scope`, or `function_scope` — so a rule never has to work out where it is.
Only a real parse can do that, and only a real parse tells a declaration apart from a constructor's mem-initializer or an aggregate at a call site.
That is the entire reason a linter this small carries a parser at all.

**Namespaces and using-directives are nodes**, because *which names are in scope here* is a scope question and rules own no scope logic.
A `namespace_definition` carries its name as written and its `body` span.
A `using_directive` carries the namespace it nominates and an `effect` span covering exactly where it is in force.
Both are answered by testing a byte offset against a span, so a rule never walks parents.

**A declarator is not a name.** A node's `declarator` span reaches through any array suffix — `b[2]`, not `b`.
The two differ exactly where a rewrite would otherwise delete the bound.

### The four judgements that keep function-scope parsing honest

Descending into function bodies is where false positives would come from, so each is decided explicitly rather than inferred.
They are named here so the boundary is findable; `parser.cc` carries the reasoning for each.

* **Mem-initializer vs function body** — only the last brace group of `S() : a{1}, b{2} {}` is the body.
* **Nested block vs initializer** — at function scope a `{` with no declarator in front of it is a block, not an initializer.
* **Statement keyword** — `return`, `throw`, `case`, … at the top of a segment disqualify it from being a declaration.
* **A type ahead of the declarator-id** — the `::`-joined name run decides whether a type is left over, which counting tokens cannot.

Lambda bodies are reached by a separate sweep, since a lambda inside a group the scanner is otherwise skipping still declares real locals.

### Statement forms, parsed as forms rather than inferred

The segment scanner above is a *declaration* scanner.
A statement whose shape the grammar fixes is parsed as that shape instead, so its parts land in the right place by construction — the same treatment `namespace` already gets.
`if` / `switch` / `while` / `for` headers are declaration scopes, a range-for splits at its `:`, and a body is exactly one statement whether or not it is braced.

### Known corner-cuts (each pinned by a corpus case)

Documented so the boundary does not regress silently:

* **Function-pointer data member with init** `void(*cb)(){…};` may mis-segment (the extra `()` reads as a parameter list).
* **`#if 0` disabled members** are still parsed as live code (directives are opaque) — a possible false positive, resolved only at the future preprocessor milestone.
* **Deeply nested statements are reached, but blocks inside a skipped group are not** — a body only becomes visible through the paths above, so an exotic construct can still hide one.
* **A binary `&&` reads as declarator punctuation.** `ok && n < T{1}` reports `T`, because `&&` is also an rvalue reference and so does not break the name run that decides whether a type is left over.
  Separating the two needs a notion of declarator position, which the parser does not have yet — the natural next increment.
* **A structured binding is invisible.** `for (auto [a, b] : m)` declares nothing the parser sees, since the `[…]` is skipped as a balanced group.
  Safe (invisible, not misread) but incomplete.

### Known corner-cuts of the prose layer

* **An indented markdown code block is read as prose.** Only fenced blocks are recognized, matching babel::markdown — a naive four-space rule collides with list-item content indentation.
* **A markdown list item's marker stays in its text.** Unlike a `//`, a `-` or `1.` sits inline with the prose, and a rule that cares is better off recognizing it than having it silently removed.
* **A Python docstring is any triple-quoted string opening a logical line.** A module-level triple-quoted constant assigned nothing is therefore documentation as far as the linter is concerned.
* **The prose heuristics are heuristics.** `no-flow-prose`'s abbreviation list grows one entry per real false positive rather than trying to be complete.

## Relationship to the clang-tidy gates

shaped-linter is the sibling of the [clang-tidy gate framework](../../lint/) — the place for rules clang-tidy structurally cannot express (e.g. macro-placement).
It shares one philosophy: **every rule carries a mandatory rationale**, printed once per run in the `rule rationale` section under the findings.
Both run **dirty-only** under `dev.py check`, so the rules adopt incrementally rather than requiring a repo-wide sweep first.

[docs/_index.md](_index.md) is the hub; [writing-a-rule](writing-a-rule.md) is the one to follow when adding a rule.
