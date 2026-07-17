# The dev.py driver — design & philosophy

This is not a how-to. For *using* the driver — building, testing, diagnosing — see
[guides/building-and-testing.md](guides/building-and-testing.md). This document explains
why [dev.py](../dev.py) and [tools/dev/](../tools/dev/) are shaped the way they are,
and how to extend the driver or adapt it in a downstream project. It is closer to a design
rationale than a tutorial: read it before you add a command, move a module, or copy the
driver into another repo.

## What the driver is for

A C++ project with several presets, compilers, and platforms accumulates a pile of
"run CMake, then Ninja, then the test binaries, then parse the output" glue. That glue
tends to rot into a single shell script nobody wants to touch. `dev.py` is the alternative:
one entry point that auto-configures, builds, tests, and *captures everything it runs* so
failures are diagnosable after the fact.

The experience it aims for:

- **Quiet by default.** Child stdout/stderr is captured to
  `build/<preset>/run-logs/`, not mirrored to the terminal. The terminal shows a terse
  colored summary; `--mirror-output` streams all of it live when you want it, and
  `--mirror-test-output` streams only the test binaries (quiet build, loud test). Mirroring is
  additive to capture, so the logs read the same either way. This is what makes the driver
  pleasant to run by hand *and* legible when an agent runs it.
- **Machine-readable sidecars.** Every step writes a JSON sidecar
  (`configure.json` / `build.json` / `test.json`) and tests write `*.results.xml`. These
  are the contract the `repo_tools` `build_diag` / `test_diag` tools read — so the loop is
  always *run `dev.py`, then diagnose from the sidecars*, never *re-run with more flags and
  squint at scrollback*.
- **Collection-oriented.** Presets and targets are selected with comma-lists, repeated
  flags, and wildcards; configure/build/test operate on *lists* of presets and targets, not
  one at a time. A toolset matrix is one invocation.
- **Colored or plain, automatically.** Color when stdout and stderr are both a terminal,
  plain when either is piped, overridable with `--colored` / `--plain` and the
  `NO_COLOR` / `FORCE_COLOR` conventions.

## The three layers

```
dev.py                 project policy + CLI wiring + dispatch     (edit to retarget)
  │  builds a Context(Policy(...)) and calls cmd.run(args, ctx)
  ▼
tools/dev/cmd/         one module per command (argparse + run)    (project commands)
  │  uses the facade for mechanism, the Context for policy/glue
  ▼
tools/dev/lib/         reusable, project-agnostic machinery       (rarely changes)
        (re-exported through the tools/dev facade)
```

**`tools/dev/lib/` — the machinery.** Plain functions over plain data: no argument parsing,
no command dispatch, no project-specific policy. Everything is collection-oriented, returns
values (`StepResult`, `Target`, records) rather than printing, and stays unopinionated about
*which* presets a project uses. It is grouped by responsibility, with a strict one-way
dependency direction (a group only imports from groups below it):

| group        | what lives there                                  | depends on            |
|--------------|---------------------------------------------------|-----------------------|
| `core`       | models, console, logs, process, archive, report   | —                     |
| `project`    | presets, targets, compdb, flags (CMake File API)  | core                  |
| `toolchain`  | toolset, llvm_tools, clangd, doctor               | core, project         |
| `pipeline`   | cmake, configure, build, test, fingerprint        | core, project, toolchain |
| `quality`    | format, crossrefs, checks                         | core                  |
| `perf`       | coverage, pgo, perf                               | core, project, toolchain, pipeline |

This is the same layering rule the C++ libraries follow: no upward or cyclic dependencies.
If two groups both want something, it belongs in a lower group. (The lone exception is a
deliberate **function-local** import — `core/process.py` reaches into `toolchain` for MSVC
env setup only when a toolset is pinned; keeping it off the module-load path preserves the
import-time layering.)

**`tools/dev/cmd/` — the commands.** One module per command (`build.py`, `test.py`,
`coverage.py`, …), each exposing exactly three things:

```python
NAME = "build"
def add_parser(sub): ...      # register this command's subparser (and any nested subcommands)
def run(args, ctx): ...       # do the work, using the facade + ctx
```

A command owns its full CLI surface — its flags live next to its logic, so copying or
adapting a command means touching one file. Commands with subcommands (`coverage`, `pgo`,
`info`, `diagnose`) build their nested subparsers inside their own `add_parser` and branch
inside their own `run`. Shared argparse fragments (`--preset`, the build-dir overrides,
`--emsdk-path`) live in [cmd/args.py](../tools/dev/cmd/args.py) so they are defined once.

**`dev.py` — policy and wiring.** What's left is small and readable: the module docstring
(the user-facing usage block), the **preset tables** (project policy — which presets each
command reaches for, per platform), the **`COMMANDS` registry** (the map of the CLI — one
import + one list entry per command), and a `main()` that builds the top-level parser, loops
over `COMMANDS` to register subparsers, parses, configures color and the optional log
archive, then constructs one `Context` and dispatches. To see the whole CLI shape and the
project's policy, you read one short file.

## The facade

[tools/dev/__init__.py](../tools/dev/__init__.py) re-exports the whole `lib/` surface as
a flat namespace: `dev.build`, `dev.report`, `dev.console`, `dev.Preset`,
`dev.discover_targets`, … Callers (commands, and downstream code) import `from tools import
dev` and reach everything through `dev.X`, without caring which group a helper lives in.

This decouples *organization* from *interface*: `lib/` can be regrouped — a module can move
from one group to another — and only the facade's import block changes; no call site moves.
The facade deliberately does **not** import `cmd` (that would be a cycle: `cmd` imports the
facade).

## Policy vs mechanism

The line the whole design defends: **`lib/` knows mechanism, `dev.py` owns policy.**

`lib/` knows *how* to resolve presets, run a build, parse JUnit. It does not know that this
project's default Windows preset is `relwithdebinfo-clang`, or that the pre-commit test gate
also runs the sanitizer preset. That knowledge is policy, and it lives in `dev.py`'s preset
tables. `dev.py` bundles those tables into a `Policy` and wraps it in a
[Context](../tools/dev/cmd/context.py) — which also carries the repo root and the
cross-command glue (preset/target resolution, the `*-test` convention, the error-exit
helper). Every command receives the `Context`, so commands never import `dev.py` and
`dev.py` never imports a command's internals; the dependency only flows one way.

## Extending the driver

**Add a command.** Create `tools/dev/cmd/<name>.py` with `NAME`, `add_parser(sub)`, and
`run(args, ctx)`, then import it and add it to the `COMMANDS` list in [dev.py](../dev.py).
Use the facade (`dev.X`) for mechanism and the `ctx` for policy/glue.

**Add a pre-commit check.** Checks are a registry in
[cmd/check.py](../tools/dev/cmd/check.py): a list of `dev.Check(name, description,
supports_fix, run, requires_green=…)`. Add one; the generic runner
(`lib/quality/checks.py`) sequences static checks first and the slow `requires_green` tail
(the test suite) only if they pass. A check's `run` prints its own summary and returns a
bool.

**Add machinery.** New reusable helper → the matching `lib/<group>/` (respecting the
dependency direction), then export it from the facade. If it would force a group to import
from a group above it, it belongs lower.

## Adapting it downstream

The split exists so another project can reuse the hard part. Copy `tools/dev/lib/`
wholesale — it carries no project-specific policy — then write your own `dev.py` (your
preset tables) and your own `tools/dev/cmd/` (the commands you need, dropping the ones you
don't). The facade and the `Context`/`Policy` seam mean your commands talk to the same
machinery this repo does.

## Design principles, in one place

- **Capture by default, diagnose from sidecars** — terse terminal, full record on disk.
- **Plain functions over plain data** — `lib/` returns values; printing and exit codes live
  in the command layer.
- **No hidden global state** — policy is passed in via `Context`, not read from module
  globals; color config is the one explicit process-wide setup.
- **One-way dependencies** — inside `lib/`, and between the three layers.
- **Composition over managers** — small modules with one responsibility, wired together,
  rather than a few large classes that know everything.
