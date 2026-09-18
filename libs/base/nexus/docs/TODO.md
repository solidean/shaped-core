# nexus TODO

Running list of known follow-ups.
The shape of what exists is in [parallel-execution](parallel-execution.md) and the [cheat-sheet](../cheat-sheet.md).

- **A blocking get inside a test body runs other tests' bodies under its name.**
  `cc::async_blocking_get` participates in the ambient pool while it waits, so the blocking thread can pick up another test's queued node and run its body.
  That body's checks are then counted for the test that blocked, and its own test reports "no CHECK/REQUIRE".
  The async ambient scope was meant to keep attribution with the node, and here it does not.
  Found when sg's transfer fuzz test swapped a parking wait for `async_blocking_get`: under `--thorough`, random sync children of the vulkan never-block sweep lost their checks in 3 of 4 runs.
  The fuzz test now runs alone under `exclusive()` as a workaround; the fix is the attribution itself, and why the stolen node's ambient does not win is not traced yet.

- **Investigate: whether an async invocation can admit an `exclusive()` child under a non-exclusive driver.**
  Today it is refused: the driver holds the phase lock shared, so a child asking for the whole phase would wait on its own driver.
  Tags are already arranged: a child's `exclusive(tag)` is taken from the phase around it, refused only when driver and child both hold tags.
  The expectation was that `exclusive()` works the same way.
  The shape to try: the invocation releases or upgrades the driver's shared hold around an exclusive child.
  sg's transfer fuzz test could then go back to being an invocable in each backend's sweep instead of a standalone test.

- **A windowed `ASYNC_EXAMPLE` in shaped-rendering**, a frame loop that awaits its work and presents on main.
  That is the case homes were built for, and nothing in the tree demonstrates it yet: `sr::window` and sg's present paths still run as they did before homes.
  The console examples in `libs/base/clean-net/examples/` and `libs/data/blob-cache/examples/` are async already.

- **A first `APP` / `COMMAND` consumer.**
  Nothing outside nexus's own wiring tests declares one yet, and no binary is registered `KINDS tool`.
  `tools/shaped-linter` is the case the design was built for: a `-core` library, a `main.cc` and a `-test` binary that collapse into one nexus binary with a `COMMAND("lint", default_entry)`.

- **The parallel batch drive never sweeps the pump registry, so an unthreaded graph cannot complete on it.**
  `tests/execute.cc` drives a batch with `cc::async_blocking_get_on`, which neither sweeps nor parks.
  `drive_serially`, in the same file, does both — and its comment states exactly why a drive that skips them cannot complete a graph waiting on a semantic thread that has no thread of its own.
  So the two paths disagree, and the parallel one is wrong.
  Any unthreaded component driven by a registered pump hits this: clean-net's reactor, and sg's own `pump_completion_signals`.
  Found from the metal side, where patching the batch path took a sweep from aborting before the first test to running all 293.
  That case has since been settled another way, but the nexus bug is unchanged.
  Giving the batch path `drive_serially`'s loop is the fix.
