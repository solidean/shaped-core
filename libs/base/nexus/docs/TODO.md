# nexus TODO

Running list of known follow-ups.
The shape of what exists is in [parallel-execution](parallel-execution.md) and the [cheat-sheet](../cheat-sheet.md).

- **A windowed `ASYNC_EXAMPLE` in shaped-rendering**, a frame loop that awaits its work and presents on main.
  That is the case homes were built for, and nothing in the tree demonstrates it yet: `sr::window` and sg's present paths still run as they did before homes.
  The console examples in `libs/base/clean-net/examples/` and `libs/data/blob-cache/examples/` are async already.

- **A first `APP` / `COMMAND` consumer.**
  Nothing outside nexus's own wiring tests declares one yet, and no binary is registered `KINDS tool`.
  `tools/shaped-linter` is the case the design was built for: a `-core` library, a `main.cc` and a `-test` binary that collapse into one nexus binary with a `COMMAND("lint", default_entry)`.

- **`SECTION` in an async body.**
  It asserts today, because the section tree is replay state and an async body runs once — see [parallel-execution](parallel-execution.md).
  A test that needs both sections and a `co_await` therefore stays sync and cannot wait on async work before it ends.
  "sv - a view accumulates across frames under its id" in `libs/graphics/shaped-viewer/tests/view-accumulation-test.cc` is the one waiting on this, and migrates once it lands.
