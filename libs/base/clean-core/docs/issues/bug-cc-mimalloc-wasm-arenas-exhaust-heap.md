---
created: 2026-11-17
priority: next
tags: [wasm, memory, mimalloc, ci]
refs:
  - file: extern/mimalloc/CMakeLists.txt
  - file: libs/base/clean-core/src/clean-core/memory/mimalloc_resource.cc
  - test: "sgl checked modules - behind a layer of the whole library*"
  - issue: bug-cc-mimalloc-wasm-threads
---

# On single-threaded wasm, mimalloc's arenas exhaust the 2 GB heap a growing vector should fit in

**The WASM (Emscripten) CI job is red on it**: `shaped-graphics-language-test` fails an allocation of 25–31 MB at alignment 64, about 3 s into the run.
The heap has reached emscripten's default 2 GB `ALLOW_MEMORY_GROWTH` ceiling by then.

## Measured

- Locally on `emscripten-relwithdebinfo`, the one test alone reproduces it: "requested [25308416, 25312512] bytes with alignment 64", after 89 of its 141 checks.
- The same build with `SC_MIMALLOC=OFF`, so emscripten's malloc behind the default resource, passes all 141 checks.
- Natively the test peaks at 473 MiB alone, and the whole binary at 1.34 GiB with tests running in parallel.
- Nothing in clean-core's allocator or the vendored mimalloc (v3.5.3, bumped 2026-10-22) changed since 2026-11-14, when this test last passed on WASM.
  The test's workload grew; the WASM job then failed at its build step until 2026-11-17, which hid the test.

## Mechanism (read from mimalloc's source, not yet confirmed by instrumenting it)

- `src/prim/emscripten/prim.c` reports no virtual reserve, so `mi_arena_reserve` reserves a quarter of `MI_DEFAULT_ARENA_RESERVE`: 32 MiB arenas on wasm32, real memory taken from emmalloc.
- A request larger than the reservation gets an arena of its own size, and v3 keeps every arena it reserved.
- A vector growing through 25–31 MB therefore needs a fresh arena at each step that the earlier, smaller ones cannot serve, and wasm memory never shrinks.
- emscripten's malloc merges freed regions, which is why it fits.

## Options, for a decision

- `SC_MIMALLOC` defaults to OFF on emscripten, as bug-cc-mimalloc-wasm-threads already advises for threaded builds; wasm gives up mimalloc's small-allocation speed.
- Build the vendored mimalloc with emscripten's own configuration of its port (64 MiB arenas, 16 KiB slices; listed in bug-cc-mimalloc-wasm-threads), or route huge requests past mimalloc on wasm.
- Raise `-sMAXIMUM_MEMORY` to 4 GB, which hides the growth rather than fixing it.

## Done when

The WASM job's `shaped-graphics-language-test` passes with the default allocator, and the chosen allocator's behavior on wasm is recorded in docs/platforms.md.
