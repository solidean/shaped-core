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
    filesystem.hh                 [done]        read_text + revision; file_revision (opaque, not a time)
    mount_table.hh/.cc            [done]        longest prefix first, newest mount first; salted revisions
    memory_filesystem.hh/.cc      [done]        write() bumps a revision — what the tests reload through
    embedded_filesystem.hh/.cc    [done]        over generator-baked files; the shipping source
    real_filesystem.hh/.cc        [done]        the one adaptor that touches the disk; mtime+size revision
    impl/path.hh/.cc              [done]        normalize/join/parent/prefix; the traversal guard
  compiler/
    shader_compiler.hh            [done]        the seam: one edge, language -> format
    dxc_compiler.hh/.cc           [done]        hlsl -> dxil via ssc::dxc; only when SLIB_HAS_DXC
  impl/
    reload_watcher.hh/.cc         [done]        cc::threaded_actor; polls revisions, stages + drives recompiles
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

**[done]:** revision polling over each asset's recorded dependencies (its source plus every resolved
include), staged recompiles promoted on the consuming thread, per-asset `generation()`, a broken edit
kept off the running shader with `last_error()`, and an unthreaded mode for `SC_THREADS=OFF` /
WebAssembly (and for deterministic tests).

**[planned] / deferred:**

- **OS file watching** instead of polling. `real_filesystem` would need a change-notification seam
  (`ReadDirectoryChangesW` / inotify / FSEvents); the watcher's diff logic would not change, since it is
  already written against `file_revision` rather than against files.
- **A timed wait on the actor.** The poll interval is a `std::this_thread::sleep_for` in 5 ms slices,
  because there is no `cc::sleep_for` and `cc::threaded_actor` has no timed wait. A `wait_for` hook on
  the actor would replace both.
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
