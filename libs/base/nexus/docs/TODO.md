# nexus TODO

Running list of known follow-ups.
The shape of what exists is in [parallel-execution](parallel-execution.md) and the [cheat-sheet](../cheat-sheet.md).

- **`ASYNC_EXAMPLE`: an example whose body may `co_await`.**
  `EXAMPLE` is a synchronous body with `main_thread` and `exclusive()` baked in, and `ASYNC_TEST` asserts against `main_thread`, so no example today can be written as a coroutine.
  The macro is `ASYNC_TEST` in the `example` bucket, with `exclusive()` and *without* `main_thread`.
  A windowed example then pins its coroutine to main with `co_await cc::async_resume_on_main()` as its first statement, which stays true across every later await.
  What it has to settle on the way: the example gallery and `dev.py example --capture` drive a synchronous body today, so capture has to wait for the graph the body returns.
  [docs/todo/cnet-waiting-on-an-async.md](../../../../docs/todo/cnet-waiting-on-an-async.md) records the same need from the clean-net side.

  **The first consumer should be a windowed example in shaped-rendering**, a frame loop that hops to main to present.
  That is the case homes were built for, and nothing in the tree demonstrates it yet: `sr::window` and sg's present paths still run as they did before homes.
  It depends on the `-j1` driver item below, since a coroutine hopping to main aborts there today.

