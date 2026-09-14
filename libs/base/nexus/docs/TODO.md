# nexus TODO

Running list of known follow-ups.
The shape of what exists is in [parallel-execution](parallel-execution.md) and the [cheat-sheet](../cheat-sheet.md).

- **A windowed `ASYNC_EXAMPLE` in shaped-rendering**, a frame loop that awaits its work and presents on main.
  That is the case homes were built for, and nothing in the tree demonstrates it yet: `sr::window` and sg's present paths still run as they did before homes.
  The console examples in `libs/base/clean-net/examples/` and `libs/data/blob-cache/examples/` are async already.

