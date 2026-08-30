# External bugs

Compiler and driver bugs we have hit, each with a **standalone reproduction** and what we concluded from it.

A bug lands here once we have stopped suspecting our own code.
Until then it is a finding about shaped-core and belongs in the library's own docs.

## Why a folder rather than a comment

A workaround in the tree says *what* we do about a bug.
It cannot say whether the bug is still there, whether it is ours, or what it takes to trigger.
Those questions come back every time a toolchain or a driver updates, and re-deriving the answer is the expensive part.

So each entry carries the answer as something runnable.

## The rules

- **One directory per bug**, kebab-case, named for the symptom rather than for the file that hit it.
- **`repro.cc`** (or the language's equivalent) depends on **nothing of ours**.
  No clean-core, no shaped-graphics, no `dev.py`, no third-party library beyond the one the bug is in.
  A reproduction that includes our headers cannot answer "is it us", which is the only question that matters.
- **`run.py`** drives the whole thing: finds the toolchain, builds, runs, prints a verdict.
  A `uv` shebang with PEP 723 metadata, matching [dev.py](../../dev.py); it must run from its own directory with no environment set up beforehand.
  Exit **1** when the bug reproduces and **0** when it does not, so it can be used as a check.
- **Minimize.** Cut until removing one more thing makes the bug disappear, then keep that thing and say why it is load-bearing.
  A repro that is only *mostly* minimal hides the trigger.
- **`readme.md`** carries what happens and **why it is not our bug**, with the matrix that isolates the trigger.
  Then how to reproduce it, what we do about it in the tree, and what to re-check when the vendor ships an update.
- **Isolate the variable.** Two compilers, two optimization levels, two vendors' drivers — whatever separates "the tool is wrong" from "we are wrong".
  That table is usually the most valuable thing in the entry.
- **Link both ways.** The workaround in the tree names this directory; this directory names the workaround.

## The entries

| bug | component | status |
|---|---|---|
| [msvc-const-ref-through-type-erased-call](msvc-const-ref-through-type-erased-call/readme.md) | MSVC `cl` 19.51 at `/O2` | filed upstream, quarantined in `function_ref-test.cc` |
| [vulkan-concurrent-device-lifecycle-deadlock](vulkan-concurrent-device-lifecycle-deadlock/readme.md) | NVIDIA Vulkan driver 591.86 | not yet filed, worked around in the vulkan backend |

## Retiring one

When a vendor ships a fix, `run.py` is what says so.
If it reports the bug gone on every configuration we care about, drop the workaround in the tree and this directory with it, in the same change.
An entry nobody can retire because nobody knows what "fixed" looks like is the failure this format exists to prevent.
