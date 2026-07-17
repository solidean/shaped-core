---
name: building-and-testing
description: This skill is for when you need to build and/or run tests for shaped-core. Drives dev.py and the repo_tools build_diag/test_diag diagnostics the way they're meant to be used.
when_to_use: "build the project", "run the tests", "run a test", "is it green", "build failed", "test failed", "compile error", "dev.py", "release build", "full test sweep"
allowed-tools: Bash mcp__repo_tools__build_diag mcp__repo_tools__test_diag Read
---

## The loop

[dev.py](../../../dev.py) is the only way to build and test — never run a test
binary directly. Run it from the repo root via `uv`, with **no output piping**
(see below).

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
uv run dev.py test               # build + run the full suite
uv run dev.py build -t <target>  # build one target
uv run dev.py doctor             # sanity-check the toolchain
```

`dev.py` is **quiet by default**: it does not mirror child output. It captures
each step (configure, build, every test binary) to `build/<preset>/run-logs/`,
writes a JSON sidecar (`configure.json` / `build.json` / `test.json`) and a
per-binary `*.results.xml`, and prints only a one-line trace per step plus a
pass/fail summary. So the loop is **dev.py, then diagnose with the MCP tools**:

| After a... | Use          | For                                                            |
|------------|--------------|----------------------------------------------------------------|
| build      | `build_diag` | compile errors/warnings, grouped per TU                       |
| test run   | `test_diag`  | failure-first results from the per-binary `*.results.xml`      |

dev.py prints the exact selector to use on failure, e.g.
`build_diag base_path="build/<preset>"` and
`test_diag path="build/<preset>/**/*.results.xml" errors_only=true`. Use those.

Full reference: [docs/guides/building-and-testing.md](../../../docs/guides/building-and-testing.md).

## Hard-won specifics

- **Don't pipe dev.py into `tail`/`head`/`grep`.** Its output is already a terse
  per-step trace plus a summary — there's nothing to trim. Worse, `… 2>&1 | tail`
  masks the result: the pipeline reports `tail`'s exit code (0), so a failed
  build/test still looks like it succeeded. Run it bare, read the summary line,
  and reach for `build_diag` / `test_diag` for detail.

- **`--preset` is a PER-SUBCOMMAND flag — it goes AFTER the subcommand.**
  `uv run dev.py test --preset release-clang`, *not* `dev.py --preset … test`.
  Only `--verbose`, `--mirror-output`, `--mirror-test-output`, `--collect-logs`,
  and `--colored` / `--plain` are global (before the subcommand).
  Presets accept comma-lists, repeated flags, and shell wildcards
  (`--preset "release-*"`). `uv run dev.py list-presets` lists them.

- **Touching `CC_ASSERT_ENABLED`-gated code? Build a `release-*` preset too.**
  The default `relwithdebinfo-*` preset has assertions **ON**; only a `release-*`
  preset has them **OFF**. A change that compiles under the default can still fail
  the assertions-off branch (e.g. a member referenced only inside a `CC_ASSERT`).

- **A crash shows up as a non-zero exit / failure XML.** dev.py synthesizes a
  JUnit result from each binary's exit code, so a binary that crashes before
  printing anything is still reported as failed. Re-run the culprit with
  `uv run dev.py --mirror-test-output test "<pattern>"` to see the live stream.
  That flag mirrors only the binary; `--mirror-output` also streams configure and
  build, which you rarely want. Same choice for watching a benchmark print its table.

## Diagnose tips

- Scope `build_diag` / `test_diag` to the right build with the selector dev.py
  printed (`base_path=` for builds, `path=…*.results.xml` for tests).
- `test_diag … errors_only=true` (with a larger `limit`) expands every failure.
- Test binaries are named `*-test` and use **nexus** (Catch2 v3 CLI compatible):
  the positional `<pattern>` is a test-name substring applied across binaries.
