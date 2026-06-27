---
name: debugging-ci
description: Diagnose a red GitHub Actions run for shaped-core — find which job failed and read the real compiler/test errors out of its diagnostics artifact with build_diag / test_diag.
when_to_use: "CI is red", "the build failed on CI", "MSVC CI fails", "why did the PR check fail", "a GitHub Actions job failed", "investigate the failing run", "download the CI artifact", "ci-diag.zip", "ci-logs.zip"
allowed-tools: Bash mcp__repo_tools__build_diag mcp__repo_tools__test_diag Read
---

## Why this skill exists

CI drives `dev.py`, which is **quiet by default** — a red job's console shows
only `build failed - diagnose with: build_diag …`, never the actual compiler or
test error. The real diagnostics live in the job's uploaded **`<platform>-diagnostics`
artifact**, not the log. So diagnosing CI is: find the failing job → download its
artifact → point `build_diag` / `test_diag` at the archive. Full background:
[docs/guides/ci.md](../../../docs/guides/ci.md).

## 1. Orient — which job, which kind of failure

```bash
gh pr checks <pr>                    # pass/fail per workflow on a PR
gh run list --branch <branch>        # recent runs on a branch
gh run view <run-id>                 # job/step summary (which step failed)
gh run view --job <job-id> --log-failed   # the failing step's console tail
```

The `--log-failed` tail tells you the **kind** of failure without the detail:
- `build failed` / `all failed (N files)` → a **compile/link** error → `build_diag`.
- a test summary like `1 of 2 test run(s) failed` → **tests** red, build green →
  `test_diag`.

## 2. Download the diagnostics artifact into `build/.tmp/<name>/`

One artifact per workflow, named `<platform>-diagnostics` (matrix legs are
suffixed with the preset). Always download into `build/.tmp/` — it's gitignored,
keeps cloud artifacts out of your real `build/<preset>` trees, and is the agreed
scratch location.

```bash
gh run download <run-id> --name windows-msvc-diagnostics --dir build/.tmp/msvc
# leaves: build/.tmp/msvc/windows-msvc-diagnostics/{ci-diag.zip,ci-logs.zip,ci-test-results.xml}
```

(Omit `--name` to grab every artifact; `gh run view <run-id>` lists their names.)

## 3. Diagnose straight off the archive — no unzip

`build_diag` and `test_diag` read **inside a `.zip`** (decoded in memory). Point
`base_path` at the archive itself:

```text
# build failures — grouped per-TU error tree, unique first-errors surfaced:
build_diag base_path="build/.tmp/msvc/windows-msvc-diagnostics/ci-diag.zip" show_tags=["error"]

# test failures — failure-first results tree, green collapsed, every failure expanded:
test_diag  base_path="build/.tmp/msvc/windows-msvc-diagnostics/ci-logs.zip" errors_only=true limit=120
```

What's in the artifact (see [docs/guides/ci.md](../../../docs/guides/ci.md) for detail):
- **`ci-diag.zip`** — every `.diag.json` compile/link sidecar → feed to `build_diag`.
- **`ci-logs.zip`** — raw run logs **plus the per-binary `*.results.xml`** → feed
  to `test_diag` (or `build_diag` for the build logs).
- **`ci-test-results.xml`** — the merged JUnit report. `test_diag` wants a
  directory or archive, so prefer `ci-logs.zip`; pass this single file only if you
  point a parser at it directly.

## 4. Clean up

```bash
rm -rf build/.tmp        # gitignored, but tidy so a later run sees no stale cloud artifacts
```

## Hard-won specifics

- **Build green, tests red is a real and different mode.** A clean `build_diag`
  (`0 errors`) with `test_diag` failures means the toolchain compiled fine but a
  binary misbehaved at runtime — common when CI runs a **newer compiler than you
  have locally**. Confirm the compiler with
  `build_diag base_path="…/ci-diag.zip" mode=flags output_filter=<tu>` (it prints
  the exact `cl.exe` / `clang++` path and version) before assuming your change is
  at fault — a runtime failure in files your change never touched is usually a
  pre-existing or compiler-version issue, not your diff.
- **Don't reproduce a build error by eye from the raw log.** `build_diag` exists
  precisely so you don't grep `/showIncludes` noise — the unique first-error per
  TU is what you want, and `--keep-going` means one red run already captured
  *every* independent error.
- **`base_path` accepts the archive directly** — never `unzip` first (the old
  docs said to; they're updated). It also accepts nested archives
  (`a.zip/b.tar.gz/dir`) and a plain extracted directory if you already have one.
