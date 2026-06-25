# CLAUDE.md

## What this repo is

**shaped-core** is a collection of foundational C++ libraries by Shaped Code.
They power SOLIDEAN, internal tools, customer projects, and experimental
research.

* C++23, built with CMake (>= 3.28), with presets per platform/compiler
  (MSVC / Clang / GCC across Windows / Linux / macOS / Android).
* [dev.py](dev.py) is the unified build & test driver, run via `uv` (Python
  3.10+). See the `building-and-testing` skill and
  [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).
* The set of libraries is **growing** — the list below is current, not
  exhaustive.

---

## Project layout

Libraries live under `libs/<category>/<lib>`. Each library is
`src/<lib>/` (colocated `.hh`/`.cc`), `tests/` (a `<lib>-test` binary), and an
optional `docs/`.

One-liner per library (more will be added over time):

* **`libs/base/clean-core`** — foundational C++ data structures, memory
  utilities, assertions, and low-level primitives (`span`, `vector`, `string`,
  `optional`, `result`, fixed containers, `function_ref`, …). Namespace `cc`.
  No dependencies.
* **`libs/base/nexus`** — lightweight C++23 test framework, Catch2 v3
  CLI–compatible (discovery, filtering, sections, JUnit XML), so IDE test
  integration works out of the box. Namespace `nx`. Depends on clean-core.

Supporting directories:

* **`tools/`** — `dev/` (reusable Python build/test modules behind
  [dev.py](dev.py)) and `bin/` (checked-in binaries, e.g.
  `diag-launcher.exe`).
* **`docs/`** — repo-wide docs; start at [docs/_index.md](docs/_index.md).
* **`dev.py`**, **`CMakeLists.txt`**, **`CMakePresets.json`** — build entry
  points.

Dependency direction: a library depends only on lower libraries (plus its own
external deps). No upward or cyclic dependencies.

```text
nexus
  ↓
clean-core
  ↓
(no dependencies)
```

---

## Hard rules

* **Dependency direction.** No upward or cyclic dependencies between libraries.
  If two libraries both want some API, it usually belongs in a lower library.
* **`.clang-format` is authoritative.** Source must not change under it (requires
  clang-format >= 21); it wins over prose docs. `.clang-tidy` is still being
  calibrated — treat its warnings as advisory guidance, not gospel.
* **Test binaries are named `*-test`.** Never run a test binary directly — go
  through `uv run dev.py test` (it auto-configures, auto-builds, discovers, and
  records results). See the `building-and-testing` skill.
* **Feature branches are mandatory** (see Git workflow) — don't commit straight
  to `main`.
* **No force-push to `main`.**

---

## Build & test (essentials)

[dev.py](dev.py) is the only way to build and test. Run from the repo root,
**without piping output** (it's terse by design):

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
uv run dev.py test               # build + run the full suite
uv run dev.py build [-t <target>]
uv run dev.py format             # clang-format all libs/ .cc/.hh in place
uv run dev.py check --fix        # run pre-commit checks, auto-fixing what's safe
uv run dev.py doctor             # sanity-check the toolchain
```

**Before committing, run `uv run dev.py check --fix`.** It runs the pre-commit
gates — clang-format (dirty-only), a repo-wide cross-reference check, and the
full test suite on the default **and** a release preset (asserts on *and* off) —
auto-applies every unambiguous fix (currently clang-format), and reports one
green/red verdict. The test tail runs only once the static checks are green;
`--no-test` skips it (handy for a docs-only re-check). `check crossrefs` /
`check format` run a single gate; `check --list` lists them. Cross-references rot
easily (rename a file and links in *other* files silently break), so that check
is always full-repo. The format check pins to `.clang-format`'s declared
clang-format major version — pass `--allow-different-version` to proceed with a
different one (warning instead of error).

`dev.py` is quiet by default: it captures each step to
`build/<preset>/run-logs/`, writes JSON sidecars
(`configure.json`/`build.json`/`test.json`), and a per-binary
`*.results.xml`. So the loop is **`dev.py`, then diagnose with the
`repo_tools` MCP tools**: `build_diag` after a build, `test_diag` after a test
run. dev.py prints the exact selector to use. Full reference:
[docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).

---

## Build flags / presets

* **C++23**, all targets 64-bit.
* The default preset is chosen by platform (`relwithdebinfo-clang` on Windows,
  `relwithdebinfo-linux-clang` on Linux, `macos-arm-llvm-relwithdebinfo` on
  macOS). Override with `--preset`, which is a **per-subcommand flag — it goes
  *after* the subcommand**: `uv run dev.py test --preset release-clang`. Only
  `--verbose` / `--mirror-output` are global (before the subcommand).
* `uv run dev.py list-presets` / `list-targets` show what's available.
* The default `relwithdebinfo-*` presets have `CC_ASSERT` **on**; `release-*`
  presets have them **off**. If you touch assertion-gated code, build a
  `release-*` preset too.

---

## Strong guidelines

Guidance, not invariants — use judgment.

* **File size ~200–800 lines.** Cohesion over count. Above ~800, usually too
  many responsibilities; two 150-line files always read together are often
  better merged.
* **Directory branching ~5–15 direct entries.** Past ~15, propose a
  responsibility-based subdirectory or merge tightly-coupled files. Split by
  topic, not alphabetically.
* **Tests stay fast.** Flag slow ones rather than landing silently.
* **Codify non-obvious edge cases as tests.** A passing test that pins a subtle
  invariant is nearly as valuable as one that catches a bug.

---

## Style preferences

The authority is [docs/coding-guidelines.md](docs/coding-guidelines.md) (design
+ conventions) and `.clang-format` (formatting). Don't duplicate them — read
them. The essentials:

* Namespaces `lower_case` (`cc`, `nx`); types / functions / variables
  `snake_case`; template parameters `UpperCase`; macros `UPPER_CASE`; private
  members `_snake_case`.
* **East const** (`T const`, `span<T const>`); pointers bind left
  (`T const* p`). 120-column limit, Allman braces, 4-space indent.
* `.hh` for headers, `.cc` for implementations; headers compile standalone.
* Prefer explicit data flow, value types, and composition over deep
  inheritance; avoid hidden global state, speculative abstraction, and large
  "manager" classes.

Comments — plain prose only:

* `///` for type/member docs, `//` for inline. **No Doxygen / Javadoc / XML-doc
  tags** (`@param`, `\return`, `<summary>`, …) — API docs aren't generated here.
* A good `///` says what a thing is *for* and calls out edge cases (zero
  handling, ownership, threading, which `result` it can fail with,
  laziness/caching). Describe resulting state, ownership, lifetime, and
  invariants — not implementation steps.
* Favor *why* over *how*; the code already shows how. Don't restate code —
  prefer comments that answer "what would surprise a competent reader here?"
  Inline comments justify unusual operations, hidden dependencies, or
  correctness constraints; delete ones that merely narrate the action.
* **Be concise.** Readable without stress, but not chatty — a long, flowing
  narration style is actively problematic. Split a comment that mixes unrelated
  concerns, or move rationale to higher-level docs.
* No comments on trivial getters / one-liners. No references to the current
  task / PR / issue — that belongs in the commit message.

Helper scripts (Python): use a uv-run shebang with PEP 723 inline dependency
metadata, matching [dev.py:1-5](dev.py#L1-L5), and invoke them as
`uv run <script>.py`.

---

## Docs

* Repo-wide docs live in [docs/](docs/_index.md). Start at
  [docs/_index.md](docs/_index.md); guides live in
  [docs/guides/](docs/guides/_index.md).
* Place new docs in the matching folder with **kebab-case** names.
* When a change touches public API, behavior, hard rules, layering, build
  flags, or the developer workflow — update the relevant doc in the same change.

---

## Agent skills

Invokable session tooling lives in [.claude/skills/](.claude/skills/). The one
worth calling out here:

* **`/building-and-testing`** — drive `dev.py` and the `repo_tools`
  `build_diag` / `test_diag` diagnostics. See
  [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).

---

## Exploring the codebase

Use the `repo_tools` MCP server (`file_structure`, `repo_view`, `repo_search`,
`repo_structure`, `build_diag`, `test_diag`) for repository exploration and code
reading. These replace shell browsing (`ls`/`gci`, `cat`/`gc`,
`grep`/`rg`/`sls`, `head`/`tail`, `find`). Prefer `repo_search` /
`repo_structure` / `file_structure` over `Grep` / `Glob` — they rank and budget
results for this repo. `Read` stays the right call for focused slices of an
on-disk file (you need it before `Edit` anyway); use `repo_view` for
commit/diff content `Read` can't reach.

Scope `repo_search` and `repo_structure` with a single gitignore-style `path`
selector — it does both subtree and filename globbing, comma-OR'd: `path=libs/`,
`path=*.hh`, or `path=libs/**/*.hh`. Subtract noise with `exclude=` (or a
`!`-prefixed term in `path`). Matching defaults to `matching=smart` (case- and
separator-insensitive, so `string_view` also finds `STRING_VIEW`); use
`matching=exact` for verbatim case-sensitive matches.

---

## Cheat sheets

Each library has a fast-recall API cheat sheet colocated with its `readme.md`
(`libs/<category>/<lib>/cheat-sheet.md`) — every important symbol with a
one-line return-type/intuition comment, plus the common gotchas. **Before doing
any code work, skim the cheat sheet for the library you're touching and its most
important dependencies.**

In practice that almost always includes the two foundational sheets:

* [clean-core](libs/base/clean-core/cheat-sheet.md) — it replaces most `std::`
  usage (`cc::vector`, `cc::string`, `cc::optional`, `cc::result`, …), so it's
  relevant to nearly any C++ change.
* [nexus](libs/base/nexus/cheat-sheet.md) — how we write tests; this repo is
  strongly test-driven, so you'll reach for it whenever you add or change tests.

See [docs/guides/cheat-sheets.md](docs/guides/cheat-sheets.md) for the format and
how to write one (keep it current when public API changes).

---

## Git workflow

* **`main` is the integration branch.** Feature branches are **mandatory** and
  namespaced per contributor by their initials:
  `u/<your-initials>/<feature>` (e.g. `u/pt/...` for Philip Trettner — `pt` is
  just *his* initials; use your own).
* **`git pull` merges, not rebases** when integrating — merge commits preserve
  the factual order parallel work landed in. On conflicts, resolve and commit
  the merge (default message is fine).
* **No force-push to `main`.**
* **Before committing, run `uv run dev.py check --fix`** (format + cross-reference
  + test gates, auto-fixing what's safe). Not a git hook — run it manually.
* **Commit attribution.** For largely Claude-generated commits, add
  `Assisted-By: Claude Code <model-id>` (e.g. `claude-opus-4-8`) — **not**
  `Co-Authored-By`. Skip for human-written or trivial agent edits.
* **Multi-line commit messages via the Bash tool** use a
  `git commit -F - <<'EOF'` heredoc — never PowerShell here-string syntax
  (`@'...'@`), which is literal in Bash and silently mangles the message.

---

## Quick reference

| Want to...                       | Look at                                                          |
|----------------------------------|------------------------------------------------------------------|
| Build & test reference           | [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) |
| Run the full suite               | `uv run dev.py test`                                              |
| Run one or a batch of tests      | `uv run dev.py test "<pattern>"`                                  |
| Build a single target            | `uv run dev.py build -t <target>`                                 |
| Compute test coverage            | `uv run dev.py coverage run` ([docs/guides/coverage.md](docs/guides/coverage.md)) |
| Format code (pre-commit)         | `uv run dev.py format --dirty-only`                              |
| Run pre-commit checks            | `uv run dev.py check --fix`                                       |
| Sanity-check the toolchain       | `uv run dev.py doctor`                                            |
| List presets / targets           | `uv run dev.py list-presets` / `list-targets`                     |
| Coding standards & conventions   | [docs/coding-guidelines.md](docs/coding-guidelines.md)           |
| Recall a library's API fast      | its `cheat-sheet.md` (e.g. [clean-core](libs/base/clean-core/cheat-sheet.md), [nexus](libs/base/nexus/cheat-sheet.md)) |
| Write a test (nexus)             | [libs/base/nexus/cheat-sheet.md](libs/base/nexus/cheat-sheet.md) + [catch2-runner-compat.md](libs/base/nexus/docs/catch2-runner-compat.md) |
| Explore the repo                 | `repo_tools` MCP (`repo_search` / `repo_structure`)              |
| All docs                         | [docs/_index.md](docs/_index.md)                                 |
