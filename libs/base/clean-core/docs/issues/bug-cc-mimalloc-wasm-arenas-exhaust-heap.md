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
- The WASM threads + WebGPU job fails the same way, on `emscripten-threads-webgpu-relwithdebinfo` run without dev.py's timeout.
  There "sgl ir - with no feature granted and the 6.6 fills, every dx12 entry of the repo's SGL stays at Shader Model 6.6" fails a 30.9 MB request.
  That binary also takes 56.6 s of its 60 s budget there, so it times out first whenever the runner is slower.
- Natively the test peaks at 473 MiB alone, and the whole binary at 1.34 GiB with tests running in parallel.
- Nothing in clean-core's allocator or the vendored mimalloc (v3.5.3, bumped 2026-10-22) changed since 2026-11-14, when this test last passed on WASM.
  The test's workload grew; the WASM job then failed at its build step until 2026-11-17, which hid the test.

## Mechanism (read from mimalloc's source, not yet confirmed by instrumenting it)

- `src/prim/emscripten/prim.c` reports no virtual reserve, so `mi_arena_reserve` reserves a quarter of `MI_DEFAULT_ARENA_RESERVE`: 32 MiB arenas on wasm32, real memory taken from emmalloc.
- A request larger than the reservation gets an arena of its own size, and v3 keeps every arena it reserved.
- A vector growing through 25–31 MB therefore needs a fresh arena at each step that the earlier, smaller ones cannot serve, and wasm memory never shrinks.
- emscripten's malloc merges freed regions, which is why it fits.

## What mimalloc buys on wasm

Measured 2026-11-17 on `emscripten-release` (Node, one thread), same machine, the default resource with `SC_MIMALLOC` on and off.
mimalloc's native lead does not carry over: about 4× faster than the system malloc on small blocks on Windows, roughly even on wasm.

`bench-alloc - mimalloc vs system, full sweep` (alloc+free cycles, M/s, mimalloc against emscripten's malloc):

| size | 16–128 B | 256 B | 512 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB |
|---|---|---|---|---|---|---|---|
| mimalloc | 41–43 | 42.7 | 43.1 | 23.2 | 11.8 | 10.0 | 7.2 |
| system | 40–43 | 30.6 | 20.5 | 20.0 | 20.6 | 20.8 | 20.6 |

Whole workloads, mimalloc on → off:

| workload | on | off |
|---|---|---|
| `bench-vdoc-loop` 200 / 2000 / 8000 ops | 8.61 / 42 / 169 ms | 8.78 / 43 / 174 ms |
| `versioned-document-test`, two runs | 864 / 872 ms | 1000 / 914 ms |
| `babel-data-test`, two runs | 119 / 120 ms | 114 / 115 ms |
| `typed-geometry-test`, two runs | 3.0 / 2.9 s | 3.1 / 3.1 s |

So mimalloc is a few percent ahead on wasm at best, behind on blocks of 4 KiB and up, and costs the heap blowup above.

## Options, for a decision

- `SC_MIMALLOC` defaults to OFF on emscripten, as bug-cc-mimalloc-wasm-threads already advises for threaded builds; wasm gives up mimalloc's small-allocation speed.
- Build the vendored mimalloc with emscripten's own configuration of its port (64 MiB arenas, 16 KiB slices; listed in bug-cc-mimalloc-wasm-threads), or route huge requests past mimalloc on wasm.
- Raise `-sMAXIMUM_MEMORY` to 4 GB, which hides the growth rather than fixing it.

## Done when

The WASM job's `shaped-graphics-language-test` passes with the default allocator, and the chosen allocator's behavior on wasm is recorded in docs/platforms.md.
