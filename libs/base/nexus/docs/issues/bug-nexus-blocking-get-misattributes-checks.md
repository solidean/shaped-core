---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/execute.cc
  - symbol: cc::async_blocking_get
---

# A blocking get inside a test body runs other tests' bodies under its name

`cc::async_blocking_get` participates in the ambient pool while it waits, so the blocking thread can pick up another test's queued node and run its body.
That body's checks are then counted for the test that blocked, and its own test reports "no CHECK/REQUIRE".
The async ambient scope was meant to keep attribution with the node, and here it does not.

Found when sg's transfer fuzz test swapped a parking wait for `async_blocking_get`: under `--thorough`, random sync children of the vulkan never-block sweep lost their checks in 3 of 4 runs.
That test has since moved to async fuzz ops and blocks nowhere, so nothing in the tree is known to hit this now.

The fix is still the attribution itself, and why the stolen node's ambient does not win is not traced yet.
