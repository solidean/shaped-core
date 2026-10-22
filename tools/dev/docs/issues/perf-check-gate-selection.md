---
created: 2026-10-22
tags: [dev.py]
refs:
  - file: tools/dev/lib/quality/checks.py
---

# check: run gates concurrently, select gates and tests by the files a change touches, and move slow tests to thorough

`dev.py check --fix` runs every gate and the whole suite on four presets, whatever the change touched.
A docs-only commit, or a new issue file, pays the same 12 minutes as a change to clean-core.

## Measured (2026-10-22, Windows, Ryzen 9 5900X, 24 threads)

A change to one design doc and the review tool's self-test, `check --fix` on `main`, about 12 minutes.

Static gates, serial, 287 s:

| gate | time |
|---|---|
| `review` (review-self-test.py) | 101 s |
| `dev-selftest` | 100 s, of which `submodules-self-test` 79 s, `issues-self-test` 29 s, `roots-self-test` 16 s |
| `crossrefs` | 24 s |
| `shaped-lint` | 13 s |
| every other gate | under 10 s each |

Tests, 423 s over 84 runs:

| preset | build | test |
|---|---|---|
| relwithdebinfo | 24 s | 53 s |
| debug-nopch | 22 s | 134 s |
| release | 17 s | 61 s |
| singlethreaded | 24 s | 87 s |

- `shaped-viewer-test` is 208 s of the 336 s of test time: 29, 94, 32 and 52 s on the four presets.
- Next are `shaped-rendering-test` and `shaped-graphics-test`, about 23 s each; the rest is small.

## Wanted

- **Gates run concurrently by default.** The static gates are independent, and the Python self-tests alone are two thirds of their time.
  Those that write files (`--fix`) need an order among themselves; the read-only ones do not.
- **Gates selected by the files a change touches.** Each gate declares what it watches, and a change touching none of it skips the gate.
  An issue file or a doc needs the prose and reference checks only; `tools/review` changes run the review self-test, `tools/dev` changes the dev self-tests.
  This is likely the largest win.
- **The same rules per test binary and per test**, thorough ones included: "run this again if anything under X, Y or Z changed".
  The rules live beside what they guard, so a new test states its own.
  A change to a library everything depends on still runs everything; the rules narrow, they never guess.
- **Slow tests move to thorough** (`nx::is_thorough()`): `shaped-viewer-test` first, above all on debug-nopch.

## Open

- Where the rules are written: per gate in the registry, per binary in CMake, per test in the test's declaration, or a file per directory.
- How a dependency is followed: a library's tests rerun when anything it links changes, from the CMake graph, or only by declared paths.
- What `--all` and CI run: everything, always, so a missing rule is caught somewhere.
