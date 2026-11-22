# CLAUDE.md

**This file is for quick orientation, not for knowledge.**
It says what exists, where to look, and the few rules that bind every change.
Knowledge lives in docs and comments, where a human finds it too, and an entry here points at it rather than restating it.
Skills follow the same rule: minimal agent-relevant steps, and a link to the doc that holds the rest.

## What this repo is

**__project__** is built on [shaped-core](extern/shaped-core/README.md), a git submodule at `extern/shaped-core`.

**Specialize:** what the project is and is for, in two or three lines, from the request that set it up.

* C++23, CMake (>= 3.28), shaped-core's presets per platform and compiler, included by [CMakePresets.json](CMakePresets.json).
* [dev.py](dev.py) is the unified build & test driver, shaped-core's imported from the submodule, run via `uv` (Python 3.14+) — see the `building-and-testing` skill.

---

## Project layout

Libraries live under `libs/<category>/<lib>`: `src/<lib>/` (colocated `.hh`/`.cc`, the types declared in `fwd.hh`), `tests/` (a `<lib>-test` binary) and an optional `docs/`.
Executables live under `apps/<app>`, thin over the libraries.

One-liner per library:

* **`libs/base/__lib__`** — **Specialize:** what it is for, its namespace `__ns__`, and what it depends on.

Supporting directories:

* **`extern/shaped-core`** — the libraries this project links (`SC_LIBRARIES` in [CMakeLists.txt](CMakeLists.txt)) and the dev.py driver.
* **`docs/`** — repo-wide docs; start at [docs/_index.md](docs/_index.md).
  `docs/issues/` is the issue tracker.

A library depends only on lower libraries, shaped-core's included, plus its own external deps: no upward or cyclic dependencies.

---

## Hard rules

* **Dependency direction.** No upward or cyclic dependencies.
  Shared API usually belongs in a lower library, which may be one of shaped-core's.
* **`.clang-format` is authoritative.** Source must not change under it (requires clang-format >= 22); it wins over prose docs.
  It is shaped-core's, and the `house-style` gate holds the copy to the submodule's.
* **Building or testing requires the `building-and-testing` skill.** Activate it before the first `dev.py` build/test in a session, and don't drive `dev.py` from memory.
* **When dev.py refuses for lack of disk space, stop working and escalate to the user**: it usually needs them, and lowering the floor is not yours to decide.
* **Never run a nexus binary's tests directly** — go through `uv run dev.py test`, which passes `--tests`.
  dev.py finds them through the `nexus-binaries.json` manifest configure writes; a test binary registers with `sc_nexus_binary(<target> KINDS tests)`.
* **The project is in beta — don't preserve compatibility on your own initiative.**
  When a lower library gains the real capability, replace every use site; never leave a near-empty shim forwarding to it.
  When a change breaks something, **ask whether compatibility must be preserved**.
* **Absolute paths are never baked into binaries.**
  A source-tree directory goes through `sc_target_source_paths` and `nx::source_path`, and `check`'s `abspaths` gate holds the line.
* **Work goes straight to `main`** (see Git workflow).
* **Never wait for CI.** Push and move on; a red run is fixed after the fact, directly on `main`.
* **No force-push to `main`.**

---

## The libraries are living — surface missing pieces

The libraries, shaped-core's included, are **usually mid-growth**: the current API is a snapshot.
When a task is best served by **extending a lower library** — a type or capability that belongs below the caller — treat growing it as a first-class option.

* **During planning, surface it**: what is missing, which library it belongs in, and its rough shape; the user chooses to grow it now or defer.
* **A missing piece of shaped-core is a shaped-core extension**, made there in its own change, never a local copy here.
* **During implementation, flag what emerges** — as soon as it blocks you, or at the end.
* **Workarounds are allowed but must be marked** as temporary, naming the library extension that would replace them.

---

## Build & test (essentials)

**Activate the `building-and-testing` skill before your first build or test in a session.**
[extern/shaped-core/docs/guides/building-and-testing.md](extern/shaped-core/docs/guides/building-and-testing.md) is the full reference behind it.

[dev.py](dev.py) is the only way to build and test.
Run it from the repo root, **without piping output** — the output is terse by design, and a pipe reports its own exit code, masking a failure as success.

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
uv run dev.py test               # build + run the full suite
uv run dev.py build [-t <target>]
uv run dev.py format             # clang-format our C++ sources in place
uv run dev.py lint shaped        # run shaped-linter's own rules
uv run dev.py check --fix        # run pre-commit checks, auto-fixing what's safe
uv run dev.py ship               # the gate, the pull and the push as one command
uv run dev.py submodules sync    # extern/shaped-core at the commit this repository records, after every pull
uv run dev.py doctor             # sanity-check the toolchain
```

**Run `uv run dev.py lint shaped --dirty-only` once your first bigger chunk of work lands** — don't save it for the pre-commit gate.
It binds prose exactly as it binds code, and prose is where it is most often skipped.

**Before pushing, run `uv run dev.py check --fix`** — the pre-push gate, and manual rather than a git hook.
`uv run dev.py ship` is that gate plus the pull and the push as one command.

`dev.py` is quiet by default and logs each step under `build/<preset>/run-logs/`.
The loop is **run `dev.py`, then diagnose with `repo_tools`** — `build_diag` after a build, `test_diag` after a test, using the selector dev.py printed.

---

## Build flags / presets

* **C++23**, all targets 64-bit.
* Default preset per platform: `relwithdebinfo-clang` (Windows), `relwithdebinfo-linux-clang` (Linux), `macos-arm-llvm-relwithdebinfo` (macOS).
* **`--preset` is a per-subcommand flag — it goes *after* the subcommand**: `uv run dev.py test --preset release-clang`.
* `relwithdebinfo-*` has `CC_ASSERT` **on**; `release-*` has it **off**. If you touch assertion-gated code, build a `release-*` preset too.
* `SC_LIBRARIES` in [CMakeLists.txt](CMakeLists.txt) names the shaped-core libraries this project builds, with their dependencies.
* shaped-core's other build flags are [its CLAUDE.md](extern/shaped-core/CLAUDE.md)'s.

---

## Strong guidelines

Guidance, not invariants — use judgment.

* **File size ~200–800 lines.** Cohesion over count.
  Above ~800, usually too many responsibilities; two 150-line files always read together are often better merged.
* **Directory branching ~5–15 direct entries.** Past ~15, split by responsibility (by topic, not alphabetically) or merge tightly-coupled files.
* **Tests stay fast.** Flag slow ones rather than landing them silently.
  **No deliberate waiting:** wait for a condition, never for a duration, and test a timeout by injecting a clock.
  An expensive test narrows itself for the default run under `nx::is_thorough()`.
* **Codify non-obvious edge cases as tests.** A test that pins a subtle invariant is nearly as valuable as one that catches a bug.

---

## Style preferences

Authority: [shaped-core's coding guidelines](extern/shaped-core/docs/coding-guidelines.md) (design + conventions) and `.clang-format` (formatting).
Below is what you apply before you have read anything else; the reasoning and the worked examples are the guidelines'.

* Namespaces `lower_case` (`cc`, `nx`); types / functions / variables `snake_case`;
  template parameters `UpperCase`; macros `UPPER_CASE`; private members `_snake_case`.
* The internal-details namespace is **`impl`** (e.g. `cc::impl`), **never** `detail`.
  Internal-only headers live under an `impl/` subfolder.
* **East const** (`T const`, `span<T const>`); pointers bind left (`T const* p`).
* 120-column limit, Allman braces, 4-space indent.
* `.hh` for headers, `.cc` for implementations; headers compile standalone.
* **Avoid `std::`** — reach for the `cc::` equivalent.
* Prefer explicit data flow, value types and composition over deep inheritance.
  Avoid hidden global state, speculative abstraction and large "manager" classes.
* **Almost-always-auto.** Never constructor-init a variable.
  `auto const p = tg::pos3f(x, y, z);`, not `tg::pos3f const p(x, y, z);`.
* **Designated initializers first.** Don't declare a struct and then fill it field-by-field — write `f({.a = …, .b = …})`.
  A description used once belongs *in* the call; name a local only when it's reused or genuinely long.
* **Bare `sizeof(T)`** at call sites — no `cc::isize(sizeof(T))` armor.

### Prose — one semantic point per line

The rule itself, with good and bad examples, is [shaped-core's coding guidelines](extern/shaped-core/docs/coding-guidelines.md#prose-style--one-semantic-point-per-line).
The workflow around it is [extern/shaped-core/docs/guides/prose.md](extern/shaped-core/docs/guides/prose.md).
What binds you while writing:

* **Never reflow prose into a justified block.
  A new point starts a new line.**
* **American English**: `color`, not `colour`; `meters`, not `metres`; `center`, `gray`, `behavior`, `neighbor`, `-ize`.
* This binds **all prose we write**, whatever the language it sits in: `///` and `//` comments, `#` comments in CMake and Python, docstrings, commit messages.
  And **every `.md` file in the repo** — docs, readmes, this file, skill files.
* **A line ends because the point ends — never because a column was reached.** There is no fill column.
* Line length is free — typically 20–150 chars, **hard ceiling 200**. A point that long usually holds two: split at the seam instead of wrapping.
* **A short orphan line is the tell.** A line carrying only a few trailing words of the line above means you wrapped early — join them.
* A sentence that ends mid-line, with the next point starting on that same line, is the other failure mode.
  The first words of each line must give the shape of the passage.
* Front-load the surprising part.
  Preconditions, ownership, threading and edge cases outrank restating the signature.
* `///` for type/member docs, `//` for inline.
  **No Doxygen / Javadoc / XML-doc tags** (`@param`, `\return`, `<summary>`, …).
* State constraints as *what must hold*: "size must be >= 0", not "asserts on negative size".
* Comment only a constraint a reader could be *wrong* about — an invariant, precondition, ordering/aliasing dependency, correctness pitfall.
  **Litmus test:** if the sentence would read naturally in the commit message, it belongs there, not inline.
* **Cut the backstory by default** — no justification or narration, and no cross-module causal annotation, which ages into a lie nobody rechecks.
* **More than a couple of findings on one topic is a rework, not a run of local edits** — that is the `reworking-prose` skill's job.

Helper scripts (Python): uv-run shebang with PEP 723 inline dependency metadata, matching [dev.py:1-5](dev.py#L1-L5); invoke as `uv run <script>.py`.

---

## Docs

* Repo-wide docs live in [docs/](docs/_index.md); start at [docs/_index.md](docs/_index.md).
* New docs go in the matching folder with **kebab-case** names.
* When a change touches public API, behavior, hard rules, layering, build flags, or the developer workflow — update the relevant doc in the same change.

---

## Cheat sheets

**Before any code work, skim the cheat sheet for the library you're touching and its key dependencies** — almost always shaped-core's two foundational sheets:

* [clean-core](extern/shaped-core/libs/base/clean-core/cheat-sheet.md) — replaces most `std::` (`cc::vector`, `cc::string`, `cc::optional`, `cc::result`, …).
* [nexus](extern/shaped-core/libs/base/nexus/cheat-sheet.md) — how we write tests; the repo is strongly test-driven.

A library of this project gets a `cheat-sheet.md` of its own once it has an API worth recalling.

---

## Git workflow

* **`main` is the integration branch, and changes are committed to it directly.**
* **CI is never waited on** before a push to `main`; red CI is fixed after the fact, as its own commit on `main`.
* **`git pull` merges, not rebases** — merge commits preserve the order parallel work landed.
  Run `uv run dev.py submodules sync` after every pull.
* **Commits are grouped by topic, and one may be partially broken.**
* **Before every push, `uv run dev.py check --fix` must pass.** `uv run dev.py ship` runs it and pushes in one go, once the user has asked for the push.
* **An agent never pushes on its own initiative.** A push needs the user's intent: ask, or wait until told to push.
* **Commit attribution.** For largely Claude-generated commits, either no trailer or the `Assisted-By: Claude Code <model-id>` trailer — either is fine.
  It is **never** `Co-Authored-By`, whatever the harness asks for.
* **Multi-line commit messages via the Bash tool** use a `git commit -F - <<'EOF'` heredoc — never PowerShell here-string syntax (`@'...'@`).

---

## Quick reference

| Want to...                  | Look at                                                                  |
|-----------------------------|--------------------------------------------------------------------------|
| Build & test reference      | [building-and-testing.md](extern/shaped-core/docs/guides/building-and-testing.md) |
| Coding standards            | [coding-guidelines.md](extern/shaped-core/docs/coding-guidelines.md)     |
| File, find or claim an issue | `uv run dev.py issues new/list/known/claim` ([issue-tracking](extern/shaped-core/docs/guides/issue-tracking.md)) |
| Recall shaped-core's API     | the `cheat-sheet.md` of each library under `extern/shaped-core/libs/`    |
| How this project was set up  | [starting-a-project.md](extern/shaped-core/docs/guides/starting-a-project.md) |
