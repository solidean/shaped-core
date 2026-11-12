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
  A call of a builtin whose record constrains control flow is the one custom type, `controlBuiltin`.
  The extension maps it to `keyword.control.sgl`, since a `keyword` token or a modifier on `function` takes the theme's plain keyword or function colour.
  Only the server can tell an atomic `max` from a plain one, so the grammar colours just the names that are unique.
* **Inlay hints**: ` : type` after every `let` that writes no type, and `-> type` before the `=>` of every function or property whose return type is inferred.
  Each inserts itself when accepted, except on a property of a type body, which takes no `-> type`.
* **Hover and go to definition**: `sgl::reference_at` resolves the name at the position through the check's side tables.
  A hover is its declaration as SGL writes it, in a `sgl` code block, then the `///` lines above the declaration.
  A definition is the declared name, which for a builtin is in its `sgl-prelude:` document.
  A swizzle, an array's `length` and `self` have a hover and no definition.
* **Hover lists the rest of an overload set**, up to eight, after a rule; `reference::overloads` holds them all.
  A program's function of a prelude function's name hides the prelude's rather than joining its set.
* **Signature help**, with `(` and `,` as triggers: `sgl::signature_help_at`.
  It reads the tokens back from the cursor to the call's paren rather than the check, so a call still being written answers.
  The candidates are the overload set the check resolved the callee to, or every function of the name where it resolved none.
  A named argument marks the parameter it names; the active signature is the resolved one while it has the argument the cursor is in.
* **References and rename**: `sgl::occurrences_at`, which resolves the name at the cursor and then every name of the same text in every file, keeping those that resolve to the same declaration.
  A rename refuses a keyword, a word that is no name, and a declaration of the prelude; `prepareRename` answers null for those.
* **Formatting**: `sgl::format_source`, one edit replacing the whole document; the blocks around a syntax error stay as written, which their diagnostic already marks.
* **Outline and folding**: `sgl::outline_of` and `sgl::folds_of`, read from the tree and the line tree alone, so a document whose check fails keeps both.
  An entry's detail is its first line as written.
* **Workspace symbols**: every open document and every module directory file that is not open, parsed per request, matched by the query's characters in order.
  It runs on the thread driving the server, since a parse of a module directory's files takes a few milliseconds.
* **Completion**, with `.` as its trigger: `sgl::completions_at`, filtered by the word the cursor ends.
  After `x.` it is what `x` has; while that line does not parse yet, `x` is read as a name in scope, since nothing checked it.
  Anywhere else it is the locals declared before the cursor in a block still open, the parameters, the document's and its module's declarations, the prelude's, and the keywords.
  An overloaded name is one item, with its first overload's signature as the detail.
* **Quick fixes**: a diagnostic the checker knows a fix for carries it as `located_diagnostic::fixes`, and `textDocument/codeAction` offers each as a `quickfix`.
  The one fix of a diagnostic is the preferred one, so an editor's "fix all" applies it; `as-precedence` is the first.
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

On a Ryzen 9 9950X3D2, release build, 2026-11-09:

* **A keystroke in shaped-viewer's `tracer_pipeline.sgl`** is a 3.6 ms analysis: the module directories read, and the document checked behind its modules' layer.
  A keystroke in a module it uses checks that module again, and relocates the ones after it in the library's chain where its interface stands: 5.6 ms in its last, 7.4 ms in its first, `scene`.
* **A test that runs to its fuel limit**, a million steps, takes 57 ms; that is what made the interpreter read the stop flag itself.

`uv run dev.py benchmark "sgl lsp"` reproduces both.

## Testing

* `uv run dev.py test sgl` runs the protocol's unit tests: framing split at every byte, positions in both encodings, incremental edits, dispatch and the lifecycle.
* It runs the language server's too, which open a document and read what the server sent.
* They drive the server by hand on the main thread, so they run the same with `SC_THREADS=OFF`.
* The extension is tested by hand, with the checklist in the [readme](../readme.md#working-on-the-client).

## Alternatives

Designs weighed and rejected, recorded so they are not proposed again:

* **A TypeScript server inside the extension, running `sgl` per edit.**
  Every edit would pay a process start and a prelude parse that the in-process server caches.
  A running test could only be stopped by killing the process, and every other editor would need its own port.
* **A third-party C++ LSP library.**
  It is an external dependency built on the standard library, with its own JSON and threading, which this repo avoids.
  None is sans-IO, which a browser host needs.

## Growth paths

Recorded so they are not re-derived:

* **Generating the LSP types** from the specification's `metaModel.json`, once the hand-written subset in `protocol/types.hh` becomes a chore.
* **Module directories** come from the `sgl.moduleDirs` setting, which the extension resolves against the workspace folder and hands over at initialization.
  Changing the setting restarts the server, which the extension does, so the directories are read anew.
  An analysis reads every file of them anew each time, and a module file open in the editor is read as its open text, unsaved edits and all.
  What it reads is parsed and checked once for the process, `sgl::shared_library_of` and `sgl::layer_of` as every compile has them, so an edit of the document checks it alone.
  An edit to an open module file, and its opening and closing, checks every other open document again.
  A relative directory is resolved against the first workspace folder only, which leaves a multi-root workspace and per-folder settings open.
  An error in a module a document reaches is shown once, on the document's `use` of that module, with the module's own location beside it.
* **An edit that changes what a module declares checks every module after it that names anything of it again**, its unchanged part included.
  Keeping what did not change is the next cut ([the compilation model](spec/incubator/compilation-model.md#separate-compilation-checked-layers)).
* **Completion from the last analysis that checked**, so `x.` completes `x` by its checked type even where the line under edit breaks its function's check.
  Today the name is looked up again by its text, which knows locals, parameters and module-level names, and not `a.b.` chains.
* **Occurrences from an index**: `occurrences_at` resolves every name of the right text, which is fine at a document's size and would want an index across many modules.
* **A formatter beyond spacing**: line breaks for an overlong line, and indentation normalized to four spaces, which the line tree would have to judge.
* **Workspace symbols from an index**, once module directories grow past what a parse per request affords.
* **A Test Explorer** view beside the gutter marks.
