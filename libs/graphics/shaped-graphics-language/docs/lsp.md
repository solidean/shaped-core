# The language server

`sgl lsp` is SGL's language server: one more command of the `sgl` tool, speaking the Language Server Protocol over stdin and stdout.
The VS Code extension in [tools/vscode-extension/](../tools/vscode-extension/) starts it, and the [readme](../readme.md#editor-support-vs-code) says how to install and use both.
This page is for whoever works on the server.

## What it does

* **Diagnostics**, pushed per document and version: every phase's, then again with each failing test once the document's tests ran.
  A diagnostic without a detail says `sgl::summary_of(kind)`, and `unreachable-code` is drawn faded.
  A note into the prelude points at a virtual `sgl-prelude:` document, which the client fetches with `sgl/preludeText`.
  The library's own `prelude/builtins.sgl` or `prelude/core.sgl`, when open, is checked in that file's place (`sgl::prelude_file_of`).
  So an edit of `core.sgl` is checked as the prelude it is.
  A user's file of the same name elsewhere is an ordinary document.
* **Semantic tokens**, the whole document at once: what `sgl::classify` says every token is, mapped onto LSP's standard names.
* **Inlay hints**: ` : type` after every `let` that writes no type, and `-> type` before the `=>` of every function or property whose return type is inferred.
  Each inserts itself when accepted, except on a property of a type body, which takes no `-> type`.
* **Tests**: after every check the server runs the document's tests itself.
  It then sends `sgl/checkResults`, every check and assert with how often it held and failed, which the extension draws as gutter marks.

## Two halves

**The library answers every question, and the server translates.**
What a token is, what type a binding got, which checks held: each is a function of the library ([architecture](architecture.md)), and nothing in the server re-derives one.
The server's own code is the translation to LSP, the scheduling, and the transport.

```
tools/sgl/lsp_command.cc        `sgl lsp`: opens the stdio host, owns the recorder, serves until `exit`
tools/sgl/lsp/                  the SGL half: analysis, the features, the handlers
tools/sgl/lsp/protocol/         the protocol half, the `sgl-lsp-protocol` library: no SGL at all
tools/sgl/lsp/tests/            both halves' tests, which `sgl` carries as a nexus binary of kind `tests`
```

**The protocol half knows no SGL, and the build enforces it.**
`sgl-lsp-protocol` links clean-core and babel-data and nothing else, so an SGL include there fails to compile.
It is what a shared LSP library would be extracted from, once a second server wants one.
UTF-16 lives only there: LSP positions count UTF-16 units unless the client offers UTF-8, and everything beneath the protocol half is UTF-8 and byte offsets.

## The core, and how it runs without threads

`lsp::server` performs no I/O: whole messages go in through `receive`, whole messages come out of `take_outgoing`, and a host moves them.
The stdio host frames them with `Content-Length`; a browser host would hand `postMessage` payloads to the same `receive`.

* **Document changes are applied inside `receive`**, before any later request is answered, as LSP requires; only answering is asynchronous.
* **A handler runs on the thread driving the server and returns a `cc::shared_async`.**
  The heavy work — checking, classifying, running tests — runs on `cc::compute_scheduler()` over an immutable `analysis`, so nothing the server holds is locked.
* **One check per document version serves every request about it**: the analysis cache hands the same async to each.
* **A completion wakes the loop.** Every request and watched job has a node that calls `cc::thread_pump_notify()` once it settled, and the server counts those as work until they ran.
* **Without threads it is the same code.** Compute is stepped by the loop, stdin is polled instead of read on a thread, and the idle loop sleeps a millisecond on the OS instead of spinning.
  The host's pump reports work while it serves, since nothing else would read stdin.

**Stale work is stopped, not just discarded.**
A new version raises the flag of the previous one's test run, which checks it between tests.
The interpreter reads it too, through `run_limits::stop`, so a test that runs long stops within a fraction of a millisecond.
A request answers `RequestCancelled` on `$/cancelRequest`.

**stdout carries the protocol and nothing else.**
The host duplicates the real stdout into a descriptor only it writes to, in binary mode, and points fd 1 at stderr, so a stray print anywhere lands in the editor's log as a line of stderr.
The command owns the recorder, since nexus's console listener would write info lines to stdout; log records reach the client as `window/logMessage`, or stderr before `initialize` was answered.

## Measured

On a Ryzen 9 7950X3D, release build:

* **Analyzing `tests/samples/helpers.sgl`** takes 0.30 ms with the prelude's parse cached, and 0.88 ms when the prelude is parsed again, which the server therefore never does.
  The prelude is still checked with every document, since `check` cannot reuse a checked one.
* **A test that runs to its fuel limit**, a million steps, takes 57 ms; that is what made the interpreter read the stop flag itself.

`uv run dev.py benchmark "sgl lsp"` reproduces both.

## Testing

* `uv run dev.py test sgl` runs the protocol's unit tests: framing split at every byte, positions in both encodings, incremental edits, dispatch and the lifecycle.
* It runs the language server's too, which open a document and read what the server sent.
* They drive the server by hand on the main thread, so they run the same with `SC_THREADS=OFF`.
* The extension is tested by hand, with the checklist in the [readme](../readme.md#working-on-the-client).

## Alternatives

Two designs were weighed and rejected:

* **A TypeScript server inside the extension, running `sgl` per edit.**
  Every edit would pay a process start and a prelude parse that the in-process server caches.
  A running test could only be stopped by killing the process, and every other editor would need its own port.
* **A third-party C++ LSP library.**
  It is an external dependency built on the standard library, with its own JSON and threading, which this repo avoids.
  None is sans-IO, which a browser host needs.

## Growth paths

Recorded so they are not re-derived:

* **Generating the LSP types** from the specification's `metaModel.json`, once the hand-written subset in `protocol/types.hh` becomes a chore.
* **Workspace files**: snapshots hold open documents only; enumerating and watching the workspace's `.sgl` files lands with modules, and so does an analysis covering every file of one.
* **Reusing a checked prelude**, which waits for modules, where an imported module is a checked thing reused the same way.
* **Hover, go-to-definition, completion and formatting**: the checker's side tables already answer the first two per expression; the library needs a position-to-node lookup.
* **A Test Explorer** view beside the gutter marks.
