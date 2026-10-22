---
created: 2026-10-22
tags: [dev.py]
refs:
  - file: tools/dev/lib/quality/checks.py
  - file: tools/dev/cmd/check.py
  - file: tools/dev/lib/pipeline/build.py
---

# check: select gates and tests by what a change touches, run gates concurrently, and push from the gate

`dev.py check --fix` runs every gate and the whole suite on four presets, whatever the change touched.
A docs-only commit, or a new issue file, pays the same 12 minutes as a change to clean-core.
Solidean will hit the same wall, so the design below is meant to transfer: nothing in it is shaped-core specific.

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

## Design (settled 2026-10-22)

### Gates declare what they watch, and there is no default

Every gate in the registry carries either `watches=[globs]` or an explicit `always=True`, and the registry refuses to load one with neither.
"Always run" is the correct default and a bad one for performance, so it is a decision the gate spells out rather than one it falls into.
A gate runs when the change touches a file its globs match.
An issue file or a doc selects the prose and reference checks only; `tools/review` selects the review self-test, `tools/dev` the dev self-tests.

`check` prints what it skipped and why, so a wrong rule is visible rather than silent.
`--all` and CI run everything, which is where a missing rule is caught.
Red CI is fixed afterwards by policy, so a slightly higher chance of it is a fair trade for ten minutes per push.

### Default tests are selected from the CMake graph, not from hand-written paths

A test binary reruns when any source of any target in its transitive link closure changed.
The CMake file API lists every target's sources and dependencies, so the rule is exact and never rots.
A clean-core edit runs everything, an sv edit runs sv alone, and a shader package is a target like any other.

Files no target owns get a fallback.
A file under a library's directory counts as that library.
Prose counts for the prose gates only.
A file nobody can attribute, or a change to `tools/cmake`, the presets or the root build files, selects everything and says so.
The rules narrow; they never guess.

### Thorough tests opt in, per test, with author-written triggers

A thorough test runs under `check` only when it declares what it reruns on, and only when that matches the change.
The declaration sits on the test, since a test has granular provenance: a boolean fuzzer names the one file implementing "M cut S", a viewer sweep names its surface tree.
Rough shape: `nx::config::rerun_on("libs/graphics/shaped-viewer/src/**")`, written by nexus into the listing sidecar it already emits, read by dev.py from there.
A glob that matches no file is an error, which is what keeps a moved file from silently disarming the trigger.
The matcher is a function rather than a glob list, so the logic can grow past globs later.

Riding thorough tests on the binary-level graph rule was considered and rejected: a clean-core edit would run every thorough test in the repo, tens of minutes for a small change.
A thorough test with no declaration runs only under `--thorough`.
Independently, `shaped-viewer-test` on debug-nopch is a quarter of the whole test tail and belongs behind `nx::is_thorough()` today.

### Gates run concurrently, with exclusion declared

The nexus model: concurrent by default, so an issue surfaces sooner, with opt-outs.
The fixing gates keep their order and run after the read-only ones have reported.
A gate that opens a GPU device declares `exclusive="gpu"`.
The same tag has to reach how dev.py schedules test binaries across presets once those run concurrently, since their GPU binaries must not.

Builds are removed from the gates rather than serialized behind a lock.
Two ninja processes in one build directory oversubscribe the cores and share `.ninja_log` and the deps database, so they corrupt each other's bookkeeping.
A gate declares `builds=[targets]`; the runner collects every selected gate's targets per preset into one `dev.build` call, and the gates run against a tree that is already built.
One well-balanced build instead of several fighting ones, the shape the test gate already has.

`dev.build` has `one_step` for exactly this, and only `build --files` uses it.
`example`, `benchmark` and `test --target` build target by target, so ninja loads the graph once per target and walks each critical path with the rest of the machine idle.
One step becomes the default, and the itemized per-target mode the opt-in; `build_diag` reads per-compile sidecars, so diagnosis loses nothing.

### `check --push`: pull, check, push, bounded

The pull is implied by correctness, and the push is what the flag adds.
dev.py knows its own verdict, which removes a documented failure mode: a check run through a wrapper reports the wrapper's exit code.

1. Refuse a dirty working tree.
   What gets pushed is commits, and this is also what keeps edits out of a running check.
2. Pull with merge.
   A conflict stops the loop with the merge in progress, as git would leave it.
3. Run `check` with selection on the local commits against the base.
4. Push.
   A non-fast-forward rejection pulls again and continues at 5.
5. After a clean merge, re-check only what the merge can break.
   Build every target in the union of both sides' closures, since a changed signature and a new caller from the other side fail here.
   Rerun only the tests in the intersection of both sides' selections, which is usually empty.
   Static gates do not rerun: theirs was linted by them.
6. Bound it to about three rounds, then stop and say `main` is moving too fast.

Under `--fix`, a rewrite that touches files stops before the push and asks for a commit.
Nothing is committed on the tool's own initiative.

### Later: memoize gate results

With selection against the branch base in place, the same machinery can key a gate's result on the hash of its watched inputs, so a rerun after a no-op merge or a docs tweak skips what already passed.
Not the first step; it is what makes the push loop nearly free.

## Open

- The exact sidecar field and nexus API for a thorough test's triggers.
- Whether the four test presets run concurrently once builds are centralized, and how the GPU tag then limits the test scheduler.
