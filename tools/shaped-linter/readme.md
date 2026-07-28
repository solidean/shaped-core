# shaped-linter

A **self-contained C++ custom linter** for shaped-core, written in C++ on shaped-core's own libraries.

It is the "custom parsing" sibling of the [clang-tidy gate framework](../lint/): where clang-tidy expresses standard checks, shaped-linter expresses **our own rules** — including ones clang-tidy structurally cannot, such as rules about specific macro placements.

**Self-contained.** No LLVM, clang tooling, or libclang. It builds its own lexer and parser.
**On shaped-core.** `cc::` (clean-core) for all data structures / strings / IO, and `nexus` for tests — deliberate dogfooding.
**Cross-platform.** Built by default in a top-level build (`SC_BUILD_TOOLS`), skipped when shaped-core is consumed via `add_subdirectory`.

Namespace `scl` (internals `scl::impl`).

## Quick start

Drive it through `dev.py`, which builds it and resolves its path — never construct build paths by hand:

```bash
uv run dev.py lint shaped              # lint the first-party C++ sources
uv run dev.py lint shaped --dirty-only # just the next commit's changed .cc/.hh
uv run dev.py lint shaped --fix        # apply the suggested fixes in place (then `dev.py format`)

uv run dev.py build -t shaped-linter   # build the tool
uv run dev.py test shaped-linter-test  # run its tests

uv run dev.py run shaped-linter <file>...   # point it at specific files (builds first)
```

It is also a `check` gate: `uv run dev.py check` runs `shaped-lint` **dirty-only** alongside the clang-tidy gates, so the rules adopt incrementally (a changed file with a brace-form initializer is flagged, the existing tree is not swept).

## Usage

```
shaped-linter [options] <file>...

  --fix            apply each finding's suggested edit back to its file in place
  --color <mode>   auto (default), always or never
  --no-color       the old spelling of --color never
  -h / --help      print usage and exit
```

A fix is a byte-range edit, so a rewrite that shortens a line leaves the continuation lines under it aligned to where the text used to be.
Run `uv run dev.py format` after a manual `--fix` sweep and clang-format puts that right.
Under `dev.py check --fix` this is already handled: the gate runs the linters before `format` precisely so their rewrites get formatted in the same pass.

`auto` colors only when stdout and stderr are both terminals, and honours `NO_COLOR` / `FORCE_COLOR`, so a redirected run carries no escapes.
The policy is `cc::console`'s, shared with instruction-tracer and dev.py.
`dev.py lint shaped` passes its own already-resolved decision through, since it captures the linter's output.

## Output

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

rule rationale
--------------

[default-init-assignment]
  prefer a consistent assignment-form initialization `T v = value;` across the codebase; …

1 finding in 1 file (1 fixable with --fix)
```

Findings print in file/line order, each rule's mandatory rationale once at the end, then a one-line summary.
**All of that is framework-level.** A rule reports a span, a message and optionally a fix or a hint, and formats nothing itself.
That holds for multi-line spans, several labels on one line, and a second labelled span in another file alike — see [docs/writing-a-rule.md](docs/writing-a-rule.md#what-you-do-not-write).

A whole-tree run batches files across several invocations, so the rationale section and the summary repeat per batch.

## Rules

Each rule carries a stable, greppable `[slug]` id (kebab-case, like clang-tidy check names) and a **mandatory rationale** printed with every finding.

| Rule | What it enforces |
|---|---|
| `default-init-assignment` | A variable's initializer uses assignment form `name = …`, not brace form `name{…}` — data members, function locals and namespace-scope variables alike. |
| `qualified-primitive` | The sized aliases (`u32`, `isize`, `byte`, …) are spelled bare, never qualified — `cc::u32`, and equally `sg::u32` through a namespace that re-exports them. |

### `fix` and `hint`

A finding can carry two kinds of rewrite, and the distinction is load-bearing:

* a **`fix`** is safe to apply unattended — wherever the rule fires, applying it compiles and preserves behavior. `--fix` applies it.
* a **`hint`** is the nicer form that only a human can sign off on, because it may fail to compile or silently change what the code means. It is **printed and never applied**, with a message saying what to weigh.

In the rendered output they are two labelled lines: `fix:` says it will be applied by `--fix`, `help:` carries the hint's reasoning with each suggested form marked `(not applied)`.
`--fix` therefore stays trustworthy across a whole-tree run, and the judgement calls still get surfaced where you can see them.
`default-init-assignment` uses both: its fix keeps the braces (`x{0}` → `x = {0}`), while its hint offers the braceless `= 0` for a data member and the `auto v = T(0)` form for a local.

## How it works

A layered pipeline, each rule declaring the highest layer it needs:

```
source_buffer ─▶ lexer ─▶ token_stream ─▶ parser ─▶ syntax_tree ─▶ rule engine ─▶ findings ─▶ renderer ─▶ reporter
```

See [docs/writing-a-rule.md](docs/writing-a-rule.md) to add a rule and [docs/architecture.md](docs/architecture.md) for how the layers fit together.

## Tests

```bash
uv run dev.py test shaped-linter-test
uv run dev.py test "shaped-linter - corpus files" -c default_init_assignment.md   # one corpus file
```

Two layers, both nexus:

* **Smoke tests** per rule (`tests/rules/<rule>-test.cc`) — the scratchpad, kept small and debuggable.
* **A markdown corpus** (`tests/rules/corpus/<rule>.md`) — ordinary prose with annotated `cpp` blocks, one invocation per file. This is where breadth lives, and adding a case needs no C++ and no CMake change.

[docs/coding-guidelines.md](docs/coding-guidelines.md) specifies the annotation format and which layer a case belongs in.

## Layout

```
src/shaped-linter/
  cli/       command-line parsing (options, usage)
  lex/       source buffers, spans, tokens, the lexer
  parse/     the recursive-descent parser and syntax tree
  rules/     the rule type, registry, engine, and concrete rules
  report/    the diagnostic renderer: snippet (source view + carets), renderer, style, reporter
  compdb/    (reserved) compile_commands.json reader
  main.cc    executable entry point
docs/        architecture, writing a rule, coding guidelines
tests/       mirrors src/, plus rules/corpus/*.md — the data-driven rule corpus
```
