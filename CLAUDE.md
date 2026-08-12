# CLAUDE.md

## What this repo is

**shaped-core** is a collection of foundational C++ libraries by Shaped Code, powering SOLIDEAN, internal tools, customer projects, and research.

* C++23, CMake (>= 3.28), presets per platform/compiler (MSVC / Clang / GCC across Windows / Linux / macOS / Android).
* [dev.py](dev.py) is the unified build & test driver, run via `uv` (Python 3.10+) — see the `building-and-testing` skill.
* The library set is **growing** — the list below is current, not exhaustive.

---

## Project layout

Libraries live under `libs/<category>/<lib>`: `src/<lib>/` (colocated `.hh`/`.cc`), `tests/` (a `<lib>-test` binary), and an optional `docs/`.

One-liner per library:

* **`libs/base/clean-core`** — foundational data structures, memory utilities, assertions, and low-level primitives (`span`, `vector`, `string`, `optional`, `result`, fixed containers,
  `function_ref`, …). Namespace `cc`. No dependencies.
* **`libs/base/nexus`** — lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML) for out-of-the-box IDE integration.
  Carries invocable (parametrized) tests, an API-sequence fuzzer, guide benchmarks and hardware counters too — its [readme](libs/base/nexus/readme.md) has the map.
  Namespace `nx`. Depends on clean-core.
* **`libs/base/typed-geometry`** — strongly-typed C++23 math & geometry.
  The `scalar_traits` seam, `vec`/`pos`/`comp`/`bivec`/`mat`/`quat` and the first `geometry/` primitives exist.
  Everything above them — transforms, queries, curves, symbolic, mesh — is planned.
  Namespace `tg`. Depends on clean-core.
  Early stage — see its [docs/structure.md](libs/base/typed-geometry/docs/structure.md) roadmap.
* **`libs/io/babel-serializer`** — serialization / deserialization of various formats.
  Each format parses into an **unopinionated native structure** (read-once, query-friendly, not for insertion), with **opinionated aggregators** ("load an image", "load a mesh") on top.
  Readers take a `cc::read_stream` and parse against its buffered window.
  The exception is one that must hand back zero-copy views of its input: `gltf` takes a `cc::pinned_data<byte const>`.
  So far: a base64 codec, JSON + markdown readers and a SQLite engine wrapper (`data/`).
  Plus Wavefront OBJ + glTF 2.0/GLB readers (`geometry/`).
  Also PNG/JPEG codecs in `babel::png` / `babel::jpg`, with the `babel::image` aggregator on top (`image/`).
  Its [docs/coding-guidelines.md](libs/io/babel-serializer/docs/coding-guidelines.md) owns those rules and the rest of babel's own conventions.
  Namespace `babel`. Depends on clean-core + typed-geometry.
  Early stage — see its [docs/structure.md](libs/io/babel-serializer/docs/structure.md) roadmap.
* **`libs/graphics/shaped-graphics`** — graphics-API wrapper: `context`, `command_list`, GPU resources, over per-backend static libs.
  dx12 and vulkan exist today (vulkan creates devices and resources but stubs its recording paths); metal/webgpu and opengl/webgl are intended tiers with no backend yet.
  Also home to the **render-routine framework** (`sg::render_routine`, per-context `ctx.routines`) — concrete routines live in shaped-rendering.
  Namespace `sg`. Depends on clean-core + typed-geometry.
  Early stage — see [docs/graphics.md](docs/graphics.md).
* **`libs/graphics/shaped-shader-compiler-dxc`** — a lean DXC wrapper: HLSL → `sg::compiled_shader` (bytecode + reflection), plus an async content-keyed cache.
  Namespace `ssc::dxc`. Depends on shaped-graphics.
  Windows-only, and built only once `extern/dxc` has fetched DXC.
* **`libs/graphics/shaped-shader-library`** — shader packages + hot reloading:
* **`libs/graphics/shaped-shader-library`** — shader packages + hot reloading:
  any target declares its shaders via `sc_add_shader_package` and gets typed C++ symbols; `acquire(ctx)` returns bytecode in a format that context accepts.
  Namespace `slib`. Depends on shaped-graphics, plus shaped-shader-compiler-dxc where DXC exists — **sg does not depend on it**.
* **`libs/graphics/shaped-rendering`** — concrete render routines on top of sg's routine framework (mipmap gen, tonemapping, texture compression, …).
  Namespace `sr`. Depends on shaped-graphics + shaped-shader-library (routines acquire their shaders through it), plus the vendored `imgui` bundle (Dear ImGui + ImPlot + ImGuizmo).
  Hosts the **Dear ImGui renderer** (`sr::imgui_context` + `sr::imgui_routine`), drawn entirely through sg — see [docs/imgui.md](libs/graphics/shaped-rendering/docs/imgui.md).
  sr is also home to the **window abstraction** (`sr::window_system` / `sr::window`) — SDL3-backed, leaking no SDL into its API, feeding `sg::swapchain_description` a native handle.
  The API is always present; without a backend (SDL3 not fetched) `window_system::try_create` fails instead of the types disappearing.
  `SR_HAS_WINDOW` (1/0) says whether a backend was compiled in.
* **`libs/graphics/shaped-viewer`** — professional, RTX-enabled visualization renderer with a dev-friendly API.
  Namespace `sv`. Depends on shaped-rendering.
  A first vertical slice today: path-traced views blitted into a window, dx12 + DXR.

Supporting directories:

* **`tools/`** — `dev/` (Python build/test machinery behind [dev.py](dev.py);
  see [docs/dev-py-driver.md](docs/dev-py-driver.md)), `bin/` (checked-in binaries, e.g. `diag-launcher.exe`), `cmake/` (repo-wide build config modules), `lint/` (the clang-tidy gate whitelist),
  `shaped-linter/` (our own C++ linter — own lexer + parser, no LLVM; see [its readme](tools/shaped-linter/readme.md) — run via `dev.py lint shaped`), and `instruction-tracer/` (a C++ tool —
  see [its readme](tools/instruction-tracer/readme.md) — that records what optimized code actually executed; drive it via `dev.py assembly trace`).
* **`docs/`** — repo-wide docs; start at [docs/_index.md](docs/_index.md).
* **`dev.py`**, **`CMakeLists.txt`**, **`CMakePresets.json`** — build entry points.

A library depends only on lower libraries (plus its own external deps). No upward or cyclic dependencies.
Each entry above names what it depends on, and the `CMakeLists.txt` files are the ground truth.

---

## Hard rules

* **Dependency direction.** No upward or cyclic dependencies.
  Shared API usually belongs in a lower library.
* **`.clang-format` is authoritative.** Source must not change under it (requires clang-format >= 22); it wins over prose docs.
  `.clang-tidy` is still being calibrated — treat its warnings as advisory, not gospel.
* **Building or testing requires the `building-and-testing` skill.** Activate it before the first `dev.py` build/test in a session, and don't drive `dev.py` from memory.
* **Test binaries are named `*-test`.** Never run one directly — go through `uv run dev.py test`.
* **Feature branches are mandatory** (see Git workflow) — don't commit to `main`.
* **No force-push to `main`.**
* **Opening a PR requires the `opening-a-pr` skill.** Activate it before any `gh pr create` — do not hand-roll the PR.

---

## The libraries are living — surface missing pieces

Every library is **usually mid-growth**: the current API is a snapshot, and some (e.g. typed-geometry) are early stage.
When a task is best served by **extending a lower library** — a type or capability that belongs in shaped-core rather than the caller — treat growing it as a first-class option.

* **During planning, surface it.** If a task wants a library extension (e.g.
  culling that really wants a `tg::frustum` plus intersection logic), call out what's missing, which library it belongs in, and its rough shape — the user chooses to grow it now or defer.
* **During implementation, flag what emerges** — as soon as it blocks you, or at the end.
* **Workarounds are allowed but must be marked** as temporary, naming the library extension that would replace them — so the user makes the grow-vs-work-around call deliberately,
  not by a silent local hack.

---

## Build & test (essentials)

**Activate the `building-and-testing` skill before your first build or test in a session.**
It carries the non-obvious rules that keep the loop correct, and [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) is the full reference behind it.

[dev.py](dev.py) is the only way to build and test.
Run it from the repo root, **without piping output** — the output is terse by design, and a pipe reports its own exit code, masking a failure as success.

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
uv run dev.py test               # build + run the full suite
uv run dev.py build [-t <target>]
uv run dev.py format             # clang-format our C++ sources in place
uv run dev.py lint clang-tidy    # run the clang-tidy whitelist gates
uv run dev.py lint shaped        # run shaped-linter's own rules
uv run dev.py check --fix        # run pre-commit checks, auto-fixing what's safe
uv run dev.py doctor             # sanity-check the toolchain
```

**Run `uv run dev.py lint shaped --dirty-only` once your first bigger chunk of work compiles** — don't save it for the pre-commit gate.
[docs/guides/prose.md](docs/guides/prose.md) is the workflow around it, including what `--fix` will and won't do for you.

**Before committing, run `uv run dev.py check --fix`** — the pre-commit gate, and manual rather than a git hook.
It runs the clang-tidy gates, shaped-linter, clang-format, a full-repo cross-reference check, then the test suite; `--no-test` skips that tail and `--list` shows the registry.
The order is a correctness property rather than a listing convention, and [Pre-commit checks](docs/guides/building-and-testing.md#pre-commit-checks) says why.

`dev.py` is quiet by default and logs each step under `build/<preset>/run-logs/`.
The loop is **run `dev.py`, then diagnose with `repo_tools`** — `build_diag` after a build, `test_diag` after a test, using the selector dev.py printed.

---

## Build flags / presets

* **C++23**, all targets 64-bit.
* Default preset per platform: `relwithdebinfo-clang` (Windows), `relwithdebinfo-linux-clang` (Linux), `macos-arm-llvm-relwithdebinfo` (macOS).
* **`--preset` is a per-subcommand flag — it goes *after* the subcommand**: `uv run dev.py test --preset release-clang`.
* `uv run dev.py list-presets` / `list-targets` show what's available.
* `relwithdebinfo-*` has `CC_ASSERT` **on**; `release-*` has it **off**. If you touch assertion-gated code, build a `release-*` preset too.
* `SC_BUILD_TESTS` / `SC_BUILD_TOOLS` gate the `*-test` binaries and `tools/`.
  Both default to ON for a top-level build (the normal flow) and OFF when shaped-core is consumed via `add_subdirectory`.
* `SC_THREADS` (default ON) is the repo-wide threading knob → clean-core's `CC_HAS_THREADS`.
  **No API is gated on it** — threaded types keep their full surface and fall back to running on the calling thread, so never `#if` a declaration away.
  OFF is a whole-build switch, never per-target, and `check` runs a `singlethreaded-*` preset so both modes stay exercised.
  See [docs/platforms.md](docs/platforms.md#threading-sc_threads).

---

## Strong guidelines

Guidance, not invariants — use judgment.

* **File size ~200–800 lines.** Cohesion over count.
  Above ~800, usually too many responsibilities; two 150-line files always read together are often better merged.
* **Directory branching ~5–15 direct entries.** Past ~15, split by responsibility (by topic, not alphabetically) or merge tightly-coupled files.
* **Tests stay fast.** Flag slow ones rather than landing them silently.
* **Codify non-obvious edge cases as tests.** A test that pins a subtle invariant is nearly as valuable as one that catches a bug.

---

## Style preferences

Authority: [docs/coding-guidelines.md](docs/coding-guidelines.md) (design + conventions) and `.clang-format` (formatting).
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
  If a linter complains, silence the check in `.clang-tidy`; don't decorate the code.

### Prose — one semantic point per line

The rule itself, with good and bad examples, is [docs/coding-guidelines.md](docs/coding-guidelines.md#prose-style--one-semantic-point-per-line).
The workflow around it — running the linter, and when a pile of findings becomes a rework — is [docs/guides/prose.md](docs/guides/prose.md).
What binds you while writing:

* **Never reflow prose into a justified block.
  A new point starts a new line.**
* This binds **all prose we write**, whatever the language it sits in: `///` and `//` comments, `#` comments in CMake and Python, docstrings, commit messages and PR descriptions.
  And **every `.md` file in the repo** — docs, readmes, cheat sheets, this file, skill files.
  Build files are not exempt either.
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
* Comment only a constraint a reader could be *wrong* about — an invariant, precondition, ordering/aliasing dependency, correctness pitfall.
  **Litmus test:** if the sentence would read naturally in the commit message or PR description, it belongs there, not inline.
* **Cut the backstory by default** — no justification or narration, and above all no cross-module causal annotation, which ages into a lie nobody rechecks.
  A genuine Chesterton's fence is rare enough on this tree (roughly 20:1 against) to be the exception you argue for.
* No comments on trivial getters / one-liners.
* **More than a couple of findings on one topic is a rework, not a run of local edits** — that is the `reworking-prose` skill's job.

### The style is evolving — don't retrofit the whole repo

Much of the codebase predates the current rules — reflowed comments, `T x(args);`, field-by-field struct filling.
That is history, not a counter-example.

* Write **new** code in the newest style, always.
* **Never proactively convert** untouched code.
* **Migrate drive-by** what you are already editing — the function *and* its doc comment.
  Editing a file doesn't mean sweeping the file; never sweep neighbors.
* Keep the guidelines **current**: a newly agreed convention is recorded in the same change, here and in `docs/coding-guidelines.md`, the library-local guidelines, and the affected cheat sheets.
* Keep wording the user deliberately set.

Helper scripts (Python): uv-run shebang with PEP 723 inline dependency metadata, matching [dev.py:1-5](dev.py#L1-L5); invoke as `uv run <script>.py`.

---

## Docs

* Repo-wide docs live in [docs/](docs/_index.md); start at [docs/_index.md](docs/_index.md), guides at [docs/guides/](docs/guides/_index.md).
* New docs go in the matching folder with **kebab-case** names.
* When a change touches public API, behavior, hard rules, layering, build flags, or the developer workflow — update the relevant doc in the same change.

---

## Agent skills

Invokable session tooling lives in [.claude/skills/](.claude/skills/). Worth calling out here:

* **`/building-and-testing`** — drive `dev.py` and the `repo_tools` `build_diag` / `test_diag` diagnostics.
  See [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md).
* **`/reworking-prose`** — rework the comments and docs around a topic wholesale, landing every rewrite in one `dev.py lint prose-apply` pass.
  Use it whenever prose findings pile up, a guideline changes, or a surface needs more documentation than it has.

---

## Exploring the codebase

Use the `repo_tools` MCP server (`file_structure`, `repo_view`, `repo_search`, `repo_structure`, `build_diag`, `test_diag`) for exploration and code reading — it replaces shell browsing (`ls`, `cat`,
`grep`/`rg`, `head`/`tail`,
`find`). Prefer `repo_search` / `repo_structure` / `file_structure` over `Grep` / `Glob` (they rank and budget for this repo). `Read` is still right for focused slices of a file;
`repo_view` for commit/diff content `Read` can't reach.

Scope `repo_search` / `repo_structure` with a single gitignore-style `path` selector — subtree and filename globbing, comma-OR'd: `path=libs/`, `path=*.hh`,
`path=libs/**/*.hh`. Subtract noise with `exclude=` (or a `!`-prefixed term).
`matching=smart` by default (case- and separator-insensitive); `matching=exact` for verbatim.

---

## Cheat sheets

Each library has a fast-recall API cheat sheet at `libs/<category>/<lib>/cheat-sheet.md`: every important symbol with a one-line return-type or intuition comment, plus common gotchas.
**Before any code work, skim the cheat sheet for the library you're touching and its key dependencies** — almost always the two foundational sheets:

* [clean-core](libs/base/clean-core/cheat-sheet.md) — replaces most `std::`
  (`cc::vector`, `cc::string`, `cc::optional`, `cc::result`, …), so relevant to nearly any C++ change.
* [nexus](libs/base/nexus/cheat-sheet.md) — how we write tests; the repo is strongly test-driven.

See [docs/guides/cheat-sheets.md](docs/guides/cheat-sheets.md) for the format and how to write one (keep it current when public API changes).

---

## Git workflow

* **`main` is the integration branch.** Feature branches are **mandatory**, namespaced per contributor: `u/<your-initials>/<feature>` (e.g. `u/pt/...` for Philip Trettner — use your own initials).
* **`git pull` merges, not rebases** — merge commits preserve the order parallel work landed.
  On conflicts, resolve and commit the merge (default message fine).
* **Before committing, run `uv run dev.py check --fix`.** Not a git hook — manual.
* **Commit attribution.** For largely Claude-generated commits add `Assisted-By: Claude Code <model-id>`, using the exact model id — **not** `Co-Authored-By`.
  Skip it for human-written or trivial agent edits.
* **Multi-line commit messages via the Bash tool** use a `git commit -F - <<'EOF'` heredoc — never PowerShell here-string syntax (`@'...'@`), which is literal in Bash and silently mangles the message.

---

## Quick reference

| Want to...                       | Look at                                                          |
|----------------------------------|------------------------------------------------------------------|
| Build & test reference           | [docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) |
| Run the full suite               | `uv run dev.py test`                                              |
| Run one or a batch of tests      | `uv run dev.py test "<pattern>"`                                  |
| Build a single target            | `uv run dev.py build -t <target>`                                 |
| Run a non-test executable        | `uv run dev.py run <target> [args…]` (builds first, forwards args, propagates the exit code) |
| Inspect compile/link flags       | `uv run dev.py info build-flags <target>` (also `link-flags`, `compile-command <file>`) |
| See a function's codegen         | `uv run dev.py assembly search/show` ([disassembly](docs/guides/disassembly.md)) |
| See what a function *actually ran* | `uv run dev.py assembly trace --target <t> --symbol <s>` ([instruction-tracer](tools/instruction-tracer/readme.md)) |
| Compute test coverage            | `uv run dev.py coverage run` ([docs/guides/coverage.md](docs/guides/coverage.md)) |
| Profile-guided optimization      | `uv run dev.py pgo run` ([docs/guides/pgo.md](docs/guides/pgo.md))               |
| Record a benchmark metric (perf) | `GUIDE_BENCHMARK` + `nx::guide` ([docs/guides/perf-results.md](docs/guides/perf-results.md)) |
| Read hardware performance counters | `uv run dev.py profiling counters` ([docs/guides/profiling.md](docs/guides/profiling.md)) |
| Format code (pre-commit)         | `uv run dev.py format --dirty-only`                              |
| Run the clang-tidy gates         | `uv run dev.py lint clang-tidy` (`--dirty-only` in `check`; gates in [tools/lint/clang-tidy-gates.yml](tools/lint/clang-tidy-gates.yml)) |
| Run shaped-linter's own rules    | `uv run dev.py lint shaped [--dirty-only] [--fix]` ([readme](tools/shaped-linter/readme.md)) |
| Bless a stdlib / platform include | add an entry to the library's `.shaped-lint.yml` ([configuration](tools/shaped-linter/docs/configuration.md)); `uv run dev.py lint bless-includes [--write]` regenerates the baseline block |
| The prose / lint workflow        | [docs/guides/prose.md](docs/guides/prose.md)                     |
| Rework a topic's comments/docs   | the `reworking-prose` skill, applied via `uv run dev.py lint prose-apply <plan> [--dry-run] [--stats]` |
| Measure a doc surface's prose    | `uv run dev.py lint prose-stats <path>...` (lines + words, per file and total) |
| Run pre-commit checks            | `uv run dev.py check --fix`                                       |
| Sanity-check the toolchain       | `uv run dev.py doctor`                                            |
| List presets / targets           | `uv run dev.py list-presets` / `list-targets`                     |
| Pin a compiler version           | `uv run dev.py build --toolset <ver>` (`list-toolsets` shows them) |
| Coding standards & conventions   | [docs/coding-guidelines.md](docs/coding-guidelines.md)           |
| Recall a library's API fast      | its `cheat-sheet.md` (e.g. [clean-core](libs/base/clean-core/cheat-sheet.md), [nexus](libs/base/nexus/cheat-sheet.md)) |
| Write a test (nexus)             | [cheat-sheet](libs/base/nexus/cheat-sheet.md), then the [docs hub](libs/base/nexus/docs/_index.md) for invocables, fuzzing and the CLI layer |
| Explore the repo                 | `repo_tools` MCP (`repo_search` / `repo_structure`)              |
| All docs                         | [docs/_index.md](docs/_index.md)                                 |
