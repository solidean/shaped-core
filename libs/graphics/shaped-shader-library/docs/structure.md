# shaped-shader-library structure (slib::)

The living roadmap for shaped-shader-library. Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started

Update the tags as the API lands. This document is design intent, not a guarantee of final API.

## Goals

- Any target declares its own **shader package** and gets typed C++ symbols for its shaders.
- **Hot reload** that never blocks a consumer and survives a broken edit.
- Shader sources reached only through a **mounted virtual filesystem** — never a raw path.
- A **compiler seam**, because more than one language and more than one target format are coming.
- A **shipping path** that needs no mode flag.

## Top-level structure

```text
src/shaped-shader-library/
  fwd.hh / all.hh                 [done]
  shader_package.hh               [done]        shader_definition + shader_package + embedded_file
  shader_asset.hh/.cc             [done]        acquire(ctx)/acquire(format); per-format pending+current
                                                slots; consumer-side promotion; generation/last_error
  shader_library.hh/.cc           [done]        mounts + compilers + read->preprocess->compile; packages;
                                                start_hot_reload/poll_hot_reload; weak alive-token
  filesystem/
    filesystem.hh                 [done]        read_text + revision + optional watch; file_revision
                                                (opaque, not a time)
    watch.hh                      [done]        watch_sink + watch_subscription; a hint to rescan, never
                                                a report of what changed
    mount_table.hh/.cc            [done]        longest prefix first, newest mount first; salted revisions;
                                                composes the mounts' watches
    memory_filesystem.hh/.cc      [done]        write() bumps a revision and fires the watch — what the
                                                tests reload through
    embedded_filesystem.hh/.cc    [done]        over generator-baked files; the shipping source; a watch
                                                that never fires
    real_filesystem.hh/.cc        [done]        the one adaptor that touches the disk; mtime+size revision
    impl/path.hh/.cc              [done]        normalize/join/parent/prefix; the traversal guard
    impl/watch_registry.hh/.cc    [done]        one cancellable sink + a prefix-keyed set of them
    impl/watch_backend.hh         [done]        the per-OS seam; nullptr where a platform has none
    impl/watch_backend_windows.cc [done]        ReadDirectoryChangesW over one IOCP + one thread
    impl/watch_backend_none.cc    [in progress] the fallback everywhere else — Linux/macOS still to come
  compiler/
    shader_compiler.hh            [done]        the seam: one edge, language -> format
    dxc_compiler.hh/.cc           [done]        hlsl -> dxil via ssc::dxc; only when SLIB_HAS_DXC
  impl/
    reload_watcher.hh/.cc         [done]        cc::threaded_actor; parks on the mailbox and lets the
                                                filesystem wake it, else polls; stages + drives recompiles
cmake/
  ShaderPackage.cmake             [done]        sc_add_shader_package + sc_finalize_shader_packages
  GenerateShaderPackage.cmake     [done]        build-time codegen: symbols, table, embedded include closure
```

## Compiler chains

Today a compiler is a **single edge**: `source_language -> sg::shader_format`, and resolution is a
direct lookup for `(package language, requested format)`.

The shape the seam is built for, and what is still `[planned]`:

- **more compilers** — HLSL→SPIR-V, Slang, GLSL, WGSL. Each is another edge; nothing else changes.
- **chains** — a shader is authored in one language but consumed as several backend formats, and the
  path may need an intermediate hop (`slang -> hlsl -> dxil`). That needs a language→language transpile
  edge and a graph search to replace the direct lookup. Call sites do not change: `acquire(ctx)` already
  asks "reach a format this context accepts", which is a path query either way.
- **fan-out helpers** — the intent is not "every shader runs on every backend" but "properly written,
  lightly annotated HLSL can". What that annotation is, and what checks it, is undesigned.

## Shipping

**Today [done]:** the generator embeds every shader source, including the transitive `#include`
closure, and bakes the absolute source dir. The library mounts the embedded copy and then the source dir
over it when that dir exists — so a dev build reads and watches the tree and a shipped binary is
self-contained, with no mode flag and no probing.

**[planned] — precompiled bytecode.** A shipped binary still compiles at startup, so DXC ships with it.
Building the bytecode at build time and embedding *that* would remove the compiler from a shipped build
entirely. The natural shape: a build-time compile step whose output plugs into `ssc::dxc::shader_cache`
as a `cc::key_value_provider` tier — the seam that exists for exactly this — with the embedded source
staying as the fallback. Note DXC is Windows-only, so a cross-platform build cannot precompile
everything itself.

## Reload

**[done]:** revision diffing over each asset's recorded dependencies (its source plus every resolved
include), staged recompiles promoted on the consuming thread, per-asset `generation()`, a broken edit
kept off the running shader with `last_error()`, and an unthreaded mode for `SC_THREADS=OFF` /
WebAssembly (and for deterministic tests).

**[done] — OS file watching.** `filesystem::watch(prefix, sink)` is an optional capability that
`mount_table` composes across mounts, and the reload watcher subscribes to the distinct parent directory
of each dependency. The notification wakes the actor's mailbox, so a watched watcher has no interval and
no periodic wakeup at all; `revision()` stays the source of truth and the diff logic never changed, which
is what makes the notification a mere hint and lets every implementation coalesce and over-fire freely.

`watch()` returning `nullopt` means "I cannot notify — poll me", and everything falls back to the old
interval scan there. That is the honest answer under `SC_THREADS=OFF` (no thread to wait on), for a
directory that does not exist, and on every platform without a backend.

**[in progress] — the other two backends.** Windows has `ReadDirectoryChangesW` over one IOCP and one
thread. **Linux (inotify)** and **macOS (FSEvents)** are next; both currently take `watch_backend_none`
and poll. Note `APPLE` also covers iOS, which has no FSEvents, and Android is inotify-flavoured Linux.

**[planned] / deferred:**

- **Partial watching.** `mount_table::watch` is all-or-nothing: if any intersecting mount cannot notify,
  the whole watch reports `nullopt` and the caller polls everything. Watching what it can and polling only
  the rest is a real refinement. In practice "cannot notify" is a platform property, so today it is
  all-or-nothing anyway.
- **Re-watching after a directory disappears.** A watched directory that is deleted fires once and then
  goes quiet; the reload watcher only re-subscribes when the *dependency set* moves, so hot reload for
  that directory stops until something else changes. Recreating a deleted source directory mid-session is
  the only way to hit it.
- **Mounting while a watch is live.** A mount added afterwards is not picked up. Registration freezes at
  `start_hot_reload` today, which is what makes that affordable.
- **A timed wait on the actor.** The polling fallback's interval is still a `std::this_thread::sleep_for`
  in 5 ms slices, because there is no `cc::sleep_for` and `cc::threaded_actor` has no timed wait. A
  `wait_for` hook on the actor would replace both. The watched path no longer needs either — it parks on
  the mailbox, which shutdown already wakes.
- **Reload for a format nobody acquired.** Deliberate: recompiling a format no one asked for burns the
  compiler on a shader that is never used.

## Deferred

- **Per-shader compile options** (defines, optimization level). `shader_source_description` carries only
  source / entry point / stage today; the DSL has nowhere to put a define.
- **Package-level include paths.** Resolution is fixed at: next to the includer, the package root, the
  mount root. A package cannot declare extra search roots.
- **A shared-include package.** A mount with no `SHADERS` entries already works via `lib.mount`, but
  there is no CMake-level way to declare "this target publishes an include-only shader library".
- **Promoting the VFS to clean-core.** This library's `filesystem` is the deliberate trial run for a
  future `cc` virtual filesystem; `real_filesystem` is the only piece that would have to move.
