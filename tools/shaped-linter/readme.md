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
uv run dev.py build -t shaped-linter
uv run dev.py test shaped-linter-test
```

## Usage

```
shaped-linter [options] <file>...

  --fix            apply each finding's suggested edit back to its file in place
  --no-color       force plain output even on a terminal
  -h / --help      print usage and exit
```

## Rules

Each rule carries a stable, greppable `[slug]` id (kebab-case, like clang-tidy check names) and a **mandatory rationale** printed with every finding.

| Rule | What it enforces |
|---|---|
| `member-default-init-assignment` | A data member's default initializer uses assignment form `name = …`, not brace form `name{…}`. |

## How it works

A layered pipeline, each rule declaring the highest layer it needs:

```
source_buffer ─▶ lexer ─▶ token_stream ─▶ parser ─▶ syntax_tree ─▶ rule engine ─▶ findings ─▶ reporter
```

See [docs/writing-a-rule.md](docs/writing-a-rule.md) to add a rule.

## Tests

```bash
uv run dev.py test shaped-linter-test
```

Two layers, both nexus: raw `TEST`s for units, and a data-driven corpus via `INVOCABLE_TEST`.

## Layout

```
src/shaped-linter/
  cli/       command-line parsing (options, usage)
  lex/       source buffers, spans, tokens, the lexer
  parse/     the recursive-descent parser and syntax tree
  rules/     the rule type, registry, engine, and concrete rules
  report/    the grouped-by-rule findings reporter
  compdb/    (reserved) compile_commands.json reader
  main.cc    executable entry point
docs/        guides (writing a rule)
tests/       mirrors src/, plus a corpus for data-driven rule tests
```
