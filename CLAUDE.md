# CLAUDE.md

## What this repo is

**shaped-core** is a collection of foundational C++ libraries by Shaped Code,
powering SOLIDEAN, internal tools, customer projects, and research.

* C++23, CMake (>= 3.28), presets per platform/compiler (MSVC / Clang / GCC
  across Windows / Linux / macOS / Android).
* [dev.py](dev.py) is the unified build & test driver, run via `uv` (Python
  3.10+). See the `building-and-testing` skill and
  [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).
* The library set is **growing** — the list below is current, not exhaustive.

---

## Project layout

Libraries live under `libs/<category>/<lib>`: `src/<lib>/` (colocated
`.hh`/`.cc`), `tests/` (a `<lib>-test` binary), and an optional `docs/`.

One-liner per library:

* **`libs/base/clean-core`** — foundational data structures, memory utilities,
  assertions, and low-level primitives (`span`, `vector`, `string`, `optional`,
  `result`, fixed containers, `function_ref`, …). Namespace `cc`. No
  dependencies.
* **`libs/base/nexus`** — lightweight C++23 test framework, Catch2 v3
  CLI–compatible (discovery, filtering, sections, JUnit XML) for out-of-the-box
  IDE integration. Namespace `nx`. Depends on clean-core.
* **`libs/base/typed-geometry`** — strongly-typed C++23 math & geometry
  (`vec`/`pos`/`comp`; `bivec`/`mat`/`quat`/transforms/geometry/mesh planned).
  Namespace `tg`. Depends on clean-core. Early stage — see its
  [docs/structure.md](libs/base/typed-geometry/docs/structure.md) roadmap.
* **`libs/graphics/shaped-graphics`** — graphics-API wrapper: `context`,
  `command_list`, GPU resources, over per-backend static libs (dx12/vulkan tier
  1; metal/webgpu tier 2; opengl/webgl legacy). Also home to the **render-routine
  framework** (`sg::render_routine`, per-context `ctx.routines`) — concrete
  routines live in shaped-rendering. Namespace `sg`. Depends on clean-core +
  typed-geometry. Early stage — see [docs/graphics.md](docs/graphics.md).
* **`libs/graphics/shaped-shader-compiler-dxc`** — a lean DXC wrapper: HLSL →
  `sg::compiled_shader` (bytecode + reflection), plus an async content-keyed
  cache. Namespace `ssc::dxc`. Depends on shaped-graphics. Windows-only, and
  built only once `extern/dxc` has fetched DXC.
* **`libs/graphics/shaped-shader-library`** — shader packages + hot reloading:
  any target declares its shaders via `sc_add_shader_package` and gets typed C++
  symbols; `acquire(ctx)` returns bytecode in a format that context accepts.
  Namespace `slib`. Depends on shaped-graphics — **sg does not depend on it**.
  The shader system's front door is
  [shaped-graphics/docs/shaders.md](libs/graphics/shaped-graphics/docs/shaders.md).
* **`libs/graphics/shaped-rendering`** — concrete render routines on top of sg's routine framework (mipmap gen, tonemapping, texture compression, …).
  Namespace `sr`. Depends on shaped-graphics + shaped-shader-library (routines acquire their shaders through it).
  No concrete routines have landed yet.
  sr is also home to the **window abstraction** (`sr::window_system` / `sr::window`) — SDL3-backed, leaking no SDL into its API, feeding `sg::swapchain_description` a native handle.
  The API is always present; without a backend (SDL3 not fetched) `window_system::try_create` fails
  instead of the types disappearing. `SR_HAS_WINDOW` (1/0) says whether a backend was compiled in.
* **`libs/graphics/shaped-viewer`** — professional, RTX-enabled visualization
  renderer with a dev-friendly API. Namespace `sv`. Depends on shaped-rendering.
  Early-stage skeleton.

Supporting directories:

* **`tools/`** — `dev/` (Python build/test machinery behind [dev.py](dev.py);
  see [docs/dev-py-driver.md](docs/dev-py-driver.md)), `bin/` (checked-in
  binaries, e.g. `diag-launcher.exe`), `cmake/` (repo-wide build config modules),
  and `instruction-tracer/` (a C++ tool — see
  [its readme](tools/instruction-tracer/readme.md) — that records what optimized
  code actually executed; drive it via `dev.py assembly trace`).
* **`docs/`** — repo-wide docs; start at [docs/_index.md](docs/_index.md).
* **`dev.py`**, **`CMakeLists.txt`**, **`CMakePresets.json`** — build entry
  points.

A library depends only on lower libraries (plus its own external deps). No
upward or cyclic dependencies.

```text
shaped-viewer → shaped-rendering → shaped-graphics (+ backends)
                                          ↑
        shaped-shader-library → shaped-shader-compiler-dxc
                                          ↓
nexus    typed-geometry  ←────────────────┘
   ↓         ↓
     clean-core
        ↓
  (no dependencies)
```

---

## Hard rules

* **Dependency direction.** No upward or cyclic dependencies. Shared API usually
  belongs in a lower library.
* **`.clang-format` is authoritative.** Source must not change under it (requires
  clang-format >= 22); it wins over prose docs. `.clang-tidy` is still being
  calibrated — treat its warnings as advisory, not gospel.
* **Building or testing requires the `building-and-testing` skill.** Activate it
  before the first `dev.py` build/test in a session — do not drive `dev.py` from
  memory (piping into `tail`/`head` masks failures; `--mirror-test-output` shows a
  binary's live stdout).
* **Test binaries are named `*-test`.** Never run one directly — go through
  `uv run dev.py test` (auto-configures, builds, discovers, records). See the
  `building-and-testing` skill.
* **Feature branches are mandatory** (see Git workflow) — don't commit to `main`.
* **No force-push to `main`.**
* **Opening a PR requires the `opening-a-pr` skill.** Activate it before any
  `gh pr create` — do not hand-roll the PR.

---

## The libraries are living — surface missing pieces

Every library is **usually mid-growth**: the current API is a snapshot, and some
(e.g. typed-geometry) are early stage. When a task is best served by **extending a
lower library** — a type or capability that belongs in shaped-core rather than the
caller — treat growing it as a first-class option.

* **During planning, surface it.** If a task wants a library extension (e.g.
  culling that really wants a `tg::frustum` plus intersection logic), call out
  what's missing, which library it belongs in, and its rough shape — the user
  chooses to grow it now or defer.
* **During implementation, flag what emerges** — as soon as it blocks you, or at
  the end.
* **Workarounds are allowed but must be marked** as temporary, naming the library
  extension that would replace them — so the user makes the grow-vs-work-around
  call deliberately, not by a silent local hack.

---

## Build & test (essentials)

**Before you build or test in a session, activate the `building-and-testing`
skill** — it carries the non-obvious rules that keep the loop correct (never pipe
`dev.py` into `tail`/`head`/`grep` — it masks the real exit code; use the global
`--mirror-test-output` to see a binary's live stdout, e.g. a benchmark's printed
table, without the build wall (`--mirror-output` streams both);
`--preset` goes *after* the subcommand; diagnose with `build_diag` / `test_diag`).
Don't reconstruct these from memory — load the skill.

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

**Before committing, run `uv run dev.py check --fix`** — the pre-commit gate:
clang-format (dirty-only, auto-fixed), a full-repo cross-reference check, then the
test suite across debug/default/release and (Linux/macOS) sanitizer presets
(asserts on and off). `--no-test` skips the tests; `check crossrefs` / `check
format` run a single gate; `check --list` lists them. The format check pins
`.clang-format`'s clang-format major version (`--allow-different-version` to
proceed anyway).

`dev.py` is quiet by default — it logs each step under `build/<preset>/run-logs/`.
The loop: **run `dev.py`, then diagnose with `repo_tools`** — `build_diag` after a
build, `test_diag` after a test (dev.py prints the exact selector). Full reference:
[docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).

---

## Build flags / presets

* **C++23**, all targets 64-bit.
* Default preset per platform: `relwithdebinfo-clang` (Windows),
  `relwithdebinfo-linux-clang` (Linux), `macos-arm-llvm-relwithdebinfo` (macOS).
  Override with `--preset`, a **per-subcommand flag — it goes *after* the
  subcommand**: `uv run dev.py test --preset release-clang`. Only `--verbose`,
  `--mirror-output` / `--mirror-test-output`, `--collect-logs` and `--colored` /
  `--plain` are global (before the subcommand).
* `uv run dev.py list-presets` / `list-targets` show what's available.
* `relwithdebinfo-*` has `CC_ASSERT` **on**; `release-*` has it **off**. If you
  touch assertion-gated code, build a `release-*` preset too.
* `SC_BUILD_TESTS` / `SC_BUILD_TOOLS` gate the `*-test` binaries and `tools/`.
  Both default to ON for a top-level build (the normal flow) and OFF when
  shaped-core is consumed via `add_subdirectory`.
* `SC_THREADS` (default ON) is the repo-wide threading knob → clean-core's
  `CC_HAS_THREADS`. OFF forces a single-threaded build even where the platform
  has threads, which is how the WASM/no-threads mode is developed natively — the
  `singlethreaded-*` presets do exactly that, and `check` runs one. No API is
  gated on it: threaded types fall back to running on the calling thread. It
  changes struct layout, so it is a whole-build switch, never per-target. See
  [docs/platforms.md](docs/platforms.md#threading-sc_threads).

---

## Strong guidelines

Guidance, not invariants — use judgment.

* **File size ~200–800 lines.** Cohesion over count. Above ~800, usually too many
  responsibilities; two 150-line files always read together are often better
  merged.
* **Directory branching ~5–15 direct entries.** Past ~15, split by responsibility
  (by topic, not alphabetically) or merge tightly-coupled files.
* **Tests stay fast.** Flag slow ones rather than landing them silently.
* **Codify non-obvious edge cases as tests.** A test that pins a subtle invariant
  is nearly as valuable as one that catches a bug.

---

## Style preferences

Authority: [docs/coding-guidelines.md](docs/coding-guidelines.md) (design + conventions) and
`.clang-format` (formatting).
Don't duplicate them — read them. Essentials:

* Namespaces `lower_case` (`cc`, `nx`); types / functions / variables `snake_case`;
  template parameters `UpperCase`; macros `UPPER_CASE`; private members `_snake_case`.
* The internal-details namespace is **`impl`** (e.g. `cc::impl`), **never** `detail`.
  Internal-only headers live under an `impl/` subfolder.
* **East const** (`T const`, `span<T const>`); pointers bind left (`T const* p`).
* 120-column limit, Allman braces, 4-space indent.
* `.hh` for headers, `.cc` for implementations; headers compile standalone.
* Prefer explicit data flow, value types and composition over deep inheritance.
  Avoid hidden global state, speculative abstraction and large "manager" classes.
* **Almost-always-auto.** Never constructor-init a variable.
  `auto const p = tg::pos3f(x, y, z);`, not `tg::pos3f const p(x, y, z);`.
* **Designated initializers first.** Don't declare a struct and then fill it field-by-field — write `f({.a = …, .b = …})`.
  A description used once belongs *in* the call — braced list for a `span`, designated init per element.
  Name a local only when it's reused or genuinely long.
* **Bare `sizeof(T)`** at call sites — no `cc::isize(sizeof(T))` armor.
  If a linter complains, silence the check in `.clang-tidy`; don't decorate the code.

### Prose — one semantic point per line

Full rule: [docs/coding-guidelines.md](docs/coding-guidelines.md#prose-style--one-semantic-point-per-line).

* **Never reflow prose into a justified block. A new point starts a new line.**
* This binds **all prose we write**, whatever the language it sits in: `///` and `//` comments, **`CMakeLists.txt` / `.cmake` `#` comments**, Python comments and docstrings, **every `.md` file in the repo** (docs, readmes, cheat sheets, this file, skill files), commit messages and PR descriptions.
  If you are writing sentences for a human, it applies — build files are not exempt.
* **A line ends because the point ends — never because a column was reached.** There is no fill column: don't wrap at 80, 100 or 120 out of habit.
* Line length is free — typically 20–150 chars, **hard ceiling 200**. A point that long usually holds two: split at the seam instead of wrapping.
  The 120-col limit binds code, not prose.
* **A short orphan line is the tell.** A line carrying only a few trailing words of the line above means you wrapped early — join them.
* A sentence that ends mid-line, with the next point starting on that same line, is the other failure mode.
  The first words of each line must give the shape of the passage.
* Front-load the surprising part.
  Preconditions, ownership, threading and edge cases outrank restating the signature.
* `///` for type/member docs, `//` for inline.
  **No Doxygen / Javadoc / XML-doc tags** (`@param`, `\return`, `<summary>`, …) — API docs aren't generated here.
* State constraints as *what must hold*: "size must be >= 0", not "asserts on negative size".
* Cut the backstory: no rationale aimed at the author, no "why we chose this", no task/PR references.
  Those go in the commit message or a higher-level doc.
* No comments on trivial getters / one-liners.

### The style is evolving — don't retrofit the whole repo

Much of the codebase predates the current rules — reflowed comments, `T x(args);`,
field-by-field struct filling.
That is history, not a counter-example.

* Write **new** code in the newest style, always.
* **Never proactively convert** untouched code.
* **Migrate drive-by** what you are already editing — the function *and* its doc comment.
  Editing a file doesn't mean sweeping the file; never sweep neighbors.
* Keep the guidelines **current**.
  A newly agreed convention is recorded in the same change: here, in `docs/coding-guidelines.md`, in the library-local guidelines, in the affected cheat sheets.
* Keep wording the user deliberately set.

Helper scripts (Python): uv-run shebang with PEP 723 inline dependency metadata, matching [dev.py:1-5](dev.py#L1-L5); invoke as `uv run <script>.py`.

---

## Docs

* Repo-wide docs live in [docs/](docs/_index.md); start at
  [docs/_index.md](docs/_index.md), guides at
  [docs/guides/](docs/guides/_index.md).
* New docs go in the matching folder with **kebab-case** names.
* When a change touches public API, behavior, hard rules, layering, build flags,
  or the developer workflow — update the relevant doc in the same change.

---

## Agent skills

Invokable session tooling lives in [.claude/skills/](.claude/skills/). Worth
calling out here:

* **`/building-and-testing`** — drive `dev.py` and the `repo_tools` `build_diag` /
  `test_diag` diagnostics. See
  [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).

---

## Exploring the codebase

Use the `repo_tools` MCP server (`file_structure`, `repo_view`, `repo_search`,
`repo_structure`, `build_diag`, `test_diag`) for exploration and code reading — it
replaces shell browsing (`ls`, `cat`, `grep`/`rg`, `head`/`tail`, `find`). Prefer
`repo_search` / `repo_structure` / `file_structure` over `Grep` / `Glob` (they
rank and budget for this repo). `Read` is still right for focused slices of a file;
`repo_view` for commit/diff content `Read` can't reach.

Scope `repo_search` / `repo_structure` with a single gitignore-style `path`
selector — subtree and filename globbing, comma-OR'd: `path=libs/`, `path=*.hh`,
`path=libs/**/*.hh`. Subtract noise with `exclude=` (or a `!`-prefixed term).
`matching=smart` by default (case- and separator-insensitive); `matching=exact`
for verbatim.

---

## Cheat sheets

Each library has a fast-recall API cheat sheet next to its `readme.md`
(`libs/<category>/<lib>/cheat-sheet.md`): every important symbol with a one-line
return-type/intuition comment, plus common gotchas. **Before any code work, skim
the cheat sheet for the library you're touching and its key dependencies** —
almost always the two foundational sheets:

* [clean-core](libs/base/clean-core/cheat-sheet.md) — replaces most `std::`
  (`cc::vector`, `cc::string`, `cc::optional`, `cc::result`, …), so relevant to
  nearly any C++ change.
* [nexus](libs/base/nexus/cheat-sheet.md) — how we write tests; the repo is
  strongly test-driven.

See [docs/guides/cheat-sheets.md](docs/guides/cheat-sheets.md) for the format and
how to write one (keep it current when public API changes).

---

## Git workflow

* **`main` is the integration branch.** Feature branches are **mandatory**,
  namespaced per contributor: `u/<your-initials>/<feature>` (e.g. `u/pt/...` for
  Philip Trettner — use your own initials).
* **`git pull` merges, not rebases** — merge commits preserve the order parallel
  work landed. On conflicts, resolve and commit the merge (default message fine).
* **Before committing, run `uv run dev.py check --fix`.** Not a git hook — manual.
* **Commit attribution.** For largely Claude-generated commits, add
  `Assisted-By: Claude Code <model-id>` (e.g. `claude-opus-4-8`) — **not**
  `Co-Authored-By`. Skip for human-written or trivial agent edits.
* **Multi-line commit messages via the Bash tool** use a `git commit -F - <<'EOF'`
  heredoc — never PowerShell here-string syntax (`@'...'@`), which is literal in
  Bash and silently mangles the message.

---

## Quick reference

| Want to...                       | Look at                                                          |
|----------------------------------|------------------------------------------------------------------|
| Build & test reference           | [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) |
| Run the full suite               | `uv run dev.py test`                                              |
| Run one or a batch of tests      | `uv run dev.py test "<pattern>"`                                  |
| Build a single target            | `uv run dev.py build -t <target>`                                 |
| Inspect compile/link flags       | `uv run dev.py info build-flags <target>` (also `link-flags`, `compile-command <file>`) |
| See a function's codegen         | `uv run dev.py assembly search/show` ([disassembly](docs/guides/disassembly.md)) |
| See what a function *actually ran* | `uv run dev.py assembly trace --target <t> --symbol <s>` ([instruction-tracer](tools/instruction-tracer/readme.md)) |
| Compute test coverage            | `uv run dev.py coverage run` ([docs/guides/coverage.md](docs/guides/coverage.md)) |
| Profile-guided optimization      | `uv run dev.py pgo run` ([docs/guides/pgo.md](docs/guides/pgo.md))               |
| Record a benchmark metric (perf) | `GUIDE_BENCHMARK` + `nx::guide` ([docs/guides/perf-results.md](docs/guides/perf-results.md)) |
| Format code (pre-commit)         | `uv run dev.py format --dirty-only`                              |
| Run pre-commit checks            | `uv run dev.py check --fix`                                       |
| Sanity-check the toolchain       | `uv run dev.py doctor`                                            |
| List presets / targets           | `uv run dev.py list-presets` / `list-targets`                     |
| Pin a compiler version           | `uv run dev.py build --toolset <ver>` (`list-toolsets` shows them) |
| Coding standards & conventions   | [docs/coding-guidelines.md](docs/coding-guidelines.md)           |
| Recall a library's API fast      | its `cheat-sheet.md` (e.g. [clean-core](libs/base/clean-core/cheat-sheet.md), [nexus](libs/base/nexus/cheat-sheet.md)) |
| Write a test (nexus)             | [libs/base/nexus/cheat-sheet.md](libs/base/nexus/cheat-sheet.md) + [catch2-runner-compat.md](libs/base/nexus/docs/catch2-runner-compat.md) |
| Explore the repo                 | `repo_tools` MCP (`repo_search` / `repo_structure`)              |
| All docs                         | [docs/_index.md](docs/_index.md)                                 |
