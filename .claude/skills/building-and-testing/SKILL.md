---
name: building-and-testing
description: This skill is for when you need to build and/or run tests for shaped-core. Drives dev.py and the repo_tools build_diag/test_diag diagnostics the way they're meant to be used.
when_to_use: "build the project", "run the tests", "run a test", "is it green", "build failed", "test failed", "compile error", "dev.py", "release build", "full test sweep"
allowed-tools: Bash mcp__repo_tools__build_diag mcp__repo_tools__test_diag Read
---

## The loop

[dev.py](../../../dev.py) is the only way to build and test — never run a test binary directly.
Run it from the repo root via `uv`, with **no output piping**.

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
uv run dev.py test <file>.cc     # …a pattern matching no test name selects by source file instead
uv run dev.py test               # build + run the full suite
uv run dev.py build -t <target>  # build one target
uv run dev.py run <target> [args…]  # build + run a NON-test executable (a tool, a sample)
uv run dev.py doctor             # sanity-check the toolchain
```

`run` is how you invoke one of our tools.
Never hand-write `build/<preset>/tools/…/foo.exe`: that pins one preset and silently runs a stale binary.
`run` builds first, mirrors the program's output, and propagates its exit code, and it refuses `*-test` targets — those are `dev.py test`.

`dev.py` is **quiet by default**: it captures each step to `build/<preset>/run-logs/` and prints only a one-line trace per step plus a pass/fail summary.
So the loop is **dev.py, then diagnose with the MCP tools**:

| After a... | Use          | For                                                            |
|------------|--------------|----------------------------------------------------------------|
| build      | `build_diag` | compile errors/warnings, grouped per TU                       |
| test run   | `test_diag`  | failure-first results from the per-binary `*.results.xml`      |

dev.py prints the exact selector to use on failure — `build_diag base_path="build/<preset>"`, or `test_diag path="build/<preset>/**/*.results.xml" errors_only=true`.
Use the one it printed rather than composing your own.

Everything below is session judgement; the mechanisms, flags and artifact formats are [docs/guides/building-and-testing.md](../../../docs/guides/building-and-testing.md)'s.

## Hard-won specifics

- **Don't pipe dev.py into `tail`/`head`/`grep`.**
  Its output is already a terse per-step trace plus a summary, so there is nothing to trim.
  Worse, `… 2>&1 | tail` masks the result: the pipeline reports `tail`'s exit code (0), so a failed build or test still looks like it succeeded.
  Run it bare, read the summary line, and reach for `build_diag` / `test_diag` for detail.

- **`--preset` is a PER-SUBCOMMAND flag — it goes AFTER the subcommand.**
  `uv run dev.py test --preset release-clang`, *not* `dev.py --preset … test`.
  Only `--verbose`, `--mirror-output`, `--mirror-test-output`, `--collect-logs` and `--colored` / `--plain` go before it; everything else goes after.
  Getting it wrong is an argparse error rather than a wrong build, so it costs a round trip, not an answer.

- **Touching `CC_ASSERT`-gated code? Build a `release-*` preset too.**
  The default `relwithdebinfo-*` preset has assertions on and only a `release-*` preset has them off, so a change that compiles under the default can still fail the assertions-off branch.
  A member referenced only inside a `CC_ASSERT` is the classic case.

- **A crash shows up as a non-zero exit and a failure XML.**
  dev.py synthesizes a JUnit result from each binary's exit code, so a binary that crashes before printing anything is still reported as failed.
  Re-run the culprit with `uv run dev.py --mirror-test-output test "<pattern>"` to see the live stream — that flag skips the build wall, which `--mirror-output` does not.

## Diagnose tips

- Scope `build_diag` / `test_diag` to the right build with the selector dev.py printed: `base_path=` for builds, `path=…*.results.xml` for tests.
- `test_diag … errors_only=true`, with a larger `limit`, expands every failure.
- The positional `<pattern>` is a test-name substring applied across binaries, or the exact name of a whole `*-test` binary.
  A pattern that matches no name anywhere is retried as a glob over the tests' **source files**, so `vector-test.cc`, `libs/base/clean-core/tests/memory` or an absolute path all select.
  That is a filter like any other: it does not open the disabled or bucket gates, so a file of benchmarks still needs `--manual` / `--guide-benchmarks`.
