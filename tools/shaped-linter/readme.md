# shaped-linter

A **self-contained custom linter** for shaped-core, written in C++ on shaped-core's own libraries.

It is the "custom parsing" sibling of the [clang-tidy gate framework](../lint/).
Where clang-tidy expresses standard checks, shaped-linter expresses **our own rules**.
That includes ones clang-tidy structurally cannot, such as rules about specific macro placements.

**Not only C++.** It lints `.cc` / `.hh`, `.py` and `.md`, because a rule about how we *write* — a comment, a docstring, a paragraph — binds all three.
Each language has its own front end; only C++ has a parser.
**Self-contained.** No LLVM, clang tooling, or libclang.
It builds its own lexers and parser.
**On shaped-core.** `cc::` (clean-core) for all data structures / strings / IO, and `nexus` for tests — deliberate dogfooding.
**Cross-platform.** Built by default in a top-level build (`SC_BUILD_TOOLS`), skipped when shaped-core is consumed via `add_subdirectory`.

Namespace `scl` (internals `scl::impl`).

## Quick start

Drive it through `dev.py`, which builds it and resolves its path — never construct build paths by hand:

```bash
uv run dev.py lint shaped              # lint the first-party sources: libs/ tools/ docs/ .claude/skills/, plus CLAUDE.md / README.md / dev.py
uv run dev.py lint shaped --dirty-only # just the next commit's changed .cc/.hh/.py/.md
uv run dev.py lint shaped --commit <rev> # the same for a commit or `A..B` range already made (a single commit means its first-parent diff)
uv run dev.py lint shaped --fix        # apply the suggested fixes in place (then `dev.py format`)

uv run dev.py build -t shaped-linter   # build the tool
uv run dev.py test shaped-linter-test  # run its tests

uv run dev.py run shaped-linter <file>...   # point it at specific files (builds first)

uv run dev.py lint prose-apply <plan> [--dry-run] [--stats]  # apply a plan of prose rewrites (see below)
uv run dev.py lint prose-stats <path>...            # how much prose files carry, before writing a plan

uv run dev.py lint bless-includes [--write]         # refill each .shaped-lint.yml's generated baseline block
```

Rules whose answer differs per library read a [`.shaped-lint.yml`](docs/configuration.md) — the per-library policy file, merged from the repo root down.
`blessed-includes` is the first of them.

It is also a `check` gate: `uv run dev.py check` runs `shaped-lint` **dirty-only** alongside the clang-tidy gates.
So the rules adopt incrementally — a changed file with a brace-form initializer is flagged, and the existing tree is not swept.
[docs/guides/prose.md](../../docs/guides/prose.md) is the guide over this tool: where each answer lives, and when a pile of findings becomes a rework rather than a run of local edits.


## Usage

```
shaped-linter [options] <file>... [-- <file>...]
shaped-linter prose apply [options] <plan>
shaped-linter prose stats [options] <file>...

  --fix                    apply each finding's suggested edit back to its file in place
  --changed-lines <file>   report PROSE findings only on the lines named there
  --color <mode>           auto (default), always or never
  --no-color               the old spelling of --color never
  -h / --help              print usage and exit

prose apply also takes:
  --dry-run                validate the plan and report, but write nothing
  --stats                  report the prose delta per file and in total
```

Everything after a `--` is taken as a file, even when it starts with a `-`.

A fix is a byte-range edit, so a rewrite that shortens a line leaves the continuation lines under it aligned to where the text used to be.
Run `uv run dev.py format` after a manual `--fix` sweep and clang-format puts that right.
Under `dev.py check --fix` this is already handled: the gate runs the linters before `format` precisely so their rewrites get formatted in the same pass.

`auto` colors only when stdout and stderr are both terminals, and honours `NO_COLOR` / `FORCE_COLOR`, so a redirected run carries no escapes.
The policy is `cc::console`'s, shared with instruction-tracer and dev.py.
`dev.py lint shaped` passes its own already-resolved decision through, since it captures the linter's output.

### `--changed-lines`, and why dirty-only is line-exact for prose

A prose finding sits on exactly one line, and that line either changed or it did not — so scoping a dirty-only run to changed lines is exact rather than a heuristic.
A code finding can be caused by a line the edit never touched, which is why the filter applies to `rule_layer::prose` rules alone.

`dev.py lint shaped --dirty-only` computes the ranges (`git diff -U0 HEAD`, plus untracked files end to end) and passes them as a spec file, one `<path>:a-b,c-d` per line.
The effect is that editing one section of a long document puts only *those lines* in the gate.
Without it, touching a paragraph of a 400-line doc drags every older violation in that file into the next commit, and a scoped job becomes an unbounded sweep.

## `prose apply` — many prose rewrites in one pass

`prose apply` exists so a whole documentation surface can be re-decided at once and land as a single invocation.

The plan names line spans and the prose to put there:

```
## libs/base/clean-core/src/clean-core/container/key_value_cache.hh
[14-17]
| /// A tiered get-or-create cache: key_value_cache over a stack of key_value_provider tiers.
| /// The tier interface is the extension seam for on-disk / networked caches.
[49-50]
[+52]
| /// Eviction is deliberately crude — see apply_bookkeeping.
```

`[a-b]` replaces those lines, `[a]` one line, `[+n]` inserts before line n, and a span with no `| ` lines deletes.
Spans ascend and may not overlap, and their line numbers are the file as you read it.
Everything after `| ` is the verbatim final line, comment marker and indentation included — the applier infers neither.
**Every line of replacement text needs its own `| ` prefix**, including one that is itself a comment.
A bare `// …` on the second line of a block parses as a plan directive, and that fails the plan before any file is read.
A bare `|` is an empty line, and the file's existing line terminator is preserved.
A markdown fenced block is editable like any other line, because the code-unchanged check does not run on markdown at all.

Two validations run over every file before anything is written, and either one rejects the whole plan.
A failing file does not stop the pass: every remaining file is still built and judged, so one run reports every *file* that is wrong rather than only the first.
Prose findings come back with carets over the **rewritten** text, since none of it is on disk yet.


* **code is unchanged** — the non-trivia token sequence must be identical to the original's.
  This is what lets a span cover a line that also holds code (`#include <memory> // std::shared_ptr`) and still permit only its trailing comment to move.
  Markdown has no lexer and is prose end to end, so it is edited freely.
* **the new prose passes the rules** — but only on lines the plan actually wrote.
  A violation the plan did not write is not the plan's to answer for, which is the same non-ripple principle as `--changed-lines`.

`--dry-run` runs both and writes nothing.

`--stats` adds the prose delta — lines and words, before and after, per file and in total:

```
shaped-linter: applied 27 edit(s) across 14 file(s)
prose lines and words, before -> after:
   62 ->  48 (-14)    810 ->  640 (-170)   libs/graphics/shaped-graphics/src/shaped-graphics/context/context.hh
  485 -> 391 (-94)   5210 -> 4180 (-1030)   total, 14 file(s)
```

Both counts are taken over extracted prose, so a `///`, a `*` leader and the code around them never register; markdown keeps its `#` and `1.` markers, as the extractor does.
The numbers are identical under `--dry-run`, because every file is rewritten in memory before any of them is written — that is what makes the delta something to check a plan against before it lands.
A file that will not lex measures as empty rather than failing the plan: this is a report, never a gate.

## `prose stats` — how much prose a surface carries

```
uv run dev.py lint prose-stats libs/graphics/shaped-graphics/docs/concepts/
```

Reports prose lines and words per file, then a total, over the same extraction `--stats` measures a delta in — so the two can never disagree.
A directory is walked for the same file kinds `lint shaped` covers.

This is what makes a rework scopeable *before* its plan exists: candidate scopes get real counts, and a line budget can be set against a number rather than a guess.
`--stats` then reports whether the plan met it.

The `reworking-prose` skill is the workflow around both: scope, concept, plan, apply, cold-reader review.

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

Findings print per invocation, so a whole-tree run repeats the rationale section and the summary once per batch of files.

## Rules

Each rule carries a stable, greppable `[slug]` id (kebab-case, like clang-tidy check names) and a **mandatory rationale** printed with every finding.

| Rule | What it enforces |
|---|---|
| `blessed-includes` | Every angle include that is not one of ours — standard library, platform SDK, third-party — is blessed by name in a [`.shaped-lint.yml`](docs/configuration.md) above the file. The default is deny; a `deny-include` entry differs only in carrying a reason that names the replacement (`<mutex>` → `clean-core/thread/mutex.hh`). An include with a path in it or ending in `.hh` is ours and never fires, and a file no config reaches is not checked at all. Hint only: swapping a header also rewrites the call sites below it. |
| `default-init-assignment` | A variable's initializer uses assignment form `name = …`, not brace form `name{…}` — data members, function locals and namespace-scope variables alike. |
| `no-flow-prose` | Prose is one semantic point per line, so a sentence ending *mid-line* is a finding — in C++ and Python comments, Python docstrings, and markdown body text alike. A heuristic and a reminder: it carries no fix, because obeying the rule means modelling the prose rather than splicing in a newline. |
| `no-long-prose-line` | A prose line over 200 characters, the hard ceiling above the otherwise free line length. A point that long almost always holds two and wants splitting at the seam. A line whose longest unbreakable run already exceeds the ceiling — a bare URL, a long path — is left alone, since no split can bring it under. |
| `qualified-type-definition` | A header defines its types qualified — `struct cc::span { … };`, `enum class cc::seek_dir : u8 { … };` — rather than opening the namespace around them. A forward declaration never fires, which is what leaves `fwd.hh` alone; `impl` and `custom` are exempt at any depth. The one type that cannot comply is an unscoped enum with no enum-base, which the grammar gives no opaque declaration. The fix moves each run of adjacent definitions out and leaves the rest inside, so a namespace of nothing but types disappears and a mixed one is split around them. It rewrites to a name that has to be declared already, normally in that library's `fwd.hh` — a missing declaration surfaces as a compile error on the next build. |
| `qualified-primitive` | The sized aliases (`u32`, `isize`, `byte`, …) are spelled bare, never qualified — `cc::u32`, and equally `sg::u32` through a namespace that re-exports them. At a `.cc`'s file scope — anonymous namespaces included — the fix adds the using-directive it needs; in a header, where that would leak into every including TU, the rule stays quiet; inside a named namespace it hints, because the answer is that library's `fwd.hh`. |

### `fix` and `hint`

A finding can carry two kinds of rewrite, and the distinction is load-bearing:

* a **`fix`** is safe to apply unattended — wherever the rule fires, applying it compiles and preserves behavior.
  `--fix` applies it.
* a **`hint`** is the nicer form that only a human can sign off on, because it may fail to compile or silently change what the code means.
  It is **printed and never applied**, with a message saying what to weigh.

In the rendered output they are two labelled lines: `fix:` says it will be applied by `--fix`, `help:` carries the hint's reasoning with each suggested form marked `(not applied)`.
`--fix` therefore stays trustworthy across a whole-tree run, and the judgement calls still get surfaced where you can see them.
`default-init-assignment` uses both: its fix keeps the braces (`x{0}` → `x = {0}`), while its hint offers the braceless `= 0` for a data member and the `auto v = T(0)` form for a local.

A fix may carry **several edits**, applied together — that is how a rewrite which only compiles once the file also gains a line gets to be a fix rather than a hint.
An edit with an **empty span** is an insertion.
Because "safe to apply unattended" is a promise about each fix on its own, the shared edit rides on every finding.
`collect_fix_edits` merges the byte-identical copies, so the line still lands exactly once.
`qualified-primitive` is the worked example: at a `.cc`'s file scope it drops the qualifier *and* splices `using namespace cc::primitive_defines;` in after the leading `#…` block.

## How it works

A layered pipeline, each rule declaring the layer it needs and the languages it applies to:

```
                           ┌─▶ parser ─▶ syntax_tree ──┐   (C++ only)
source_buffer ─▶ lexer ─▶ token_stream                 ├─▶ rule engine ─▶ findings ─▶ renderer ─▶ reporter
              └──────────▶ prose extraction ─▶ prose_view ┘
```

The file's extension picks the front end, in one place, and the engine builds only what an enabled rule asked for.
A rule never sees a language it did not declare, so the C++ rules are structurally safe from ever meeting a markdown file.

[docs/_index.md](docs/_index.md) is the docs hub.
[writing-a-rule](docs/writing-a-rule.md) is the one to follow when adding a rule; [architecture](docs/architecture.md) covers how the layers fit together.
[coding-guidelines](docs/coding-guidelines.md) owns the tool's own conventions, including the corpus annotation format.

## Tests

```bash
uv run dev.py test shaped-linter-test
uv run dev.py test "shaped-linter - corpus files" -c cpp-style/default-init-assignment/default_init_assignment.md   # one corpus file
```

Two layers, both nexus:

* **Smoke tests** per rule (`<rule>-test.cc`, in the rule's folder) — small, debuggable, and where an interesting regression gets pinned.
* **A markdown corpus** (`<rule>.md`, in the same folder) — ordinary prose with annotated `cpp` blocks, one invocation per file.

[docs/coding-guidelines.md](docs/coding-guidelines.md) specifies the annotation format and which layer a case belongs in.

## Layout

```
src/shaped-linter/   the framework the rules stand on
  cli/       command-line parsing (options, usage)
  lex/       source buffers, spans, tokens, the language dispatch, and the three front ends
  parse/     the recursive-descent parser and syntax tree (C++ only)
  prose/     the comments, docstrings and body text a prose rule walks, plus the `prose apply` plan and applier
  rules/     the rule and finding types, the registry, the engine
  report/    the diagnostic renderer: snippet (source view + carets), renderer, style, reporter
  main.cc    executable entry point
rules/       the rules themselves, one folder each: <group>/<rule>/{rule.hh,.cc,-test.cc,.md}
docs/        _index, architecture, writing a rule, coding guidelines
tests/       mirrors src/ — the framework's own tests
```

**A rule is a folder**, so a slug read off a finding is the path to everything about it.
Nothing about a rule sits anywhere else except one line in `registry.cc` and its file names in the group's `CMakeLists.txt`.
