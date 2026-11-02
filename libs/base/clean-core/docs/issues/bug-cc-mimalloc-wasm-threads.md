---
created: 2026-10-25
tags: [wasm, memory, mimalloc]
refs:
  - file: extern/mimalloc/CMakeLists.txt
  - file: extern/mimalloc/dependency.yml
  - file: libs/base/clean-core/src/clean-core/memory/mimalloc_resource.cc
---

# The vendored mimalloc misbehaves in threaded wasm builds: memory blows up, and allocations fault after growth

Seen 2026-10-25 in a consumer's threaded wasm build (emscripten 6.0.9, Node 24), with SC_MIMALLOC on.
Both effects vanish with SC_MIMALLOC off, under emscripten's dlmalloc or its own mimalloc port (`-sMALLOC=mimalloc`).

- **Memory**: a workload that peaks near 610 MB with either of emscripten's allocators, and near 950 MB natively, exhausted the full 4 GB under the vendored mimalloc.
  With 1 thread as with 32.
- **Faults**: a test of 8 threads attaching small 64-byte-aligned blocks to shared nodes trapped `memory access out of bounds`, in 3 and 4 of 40 runs.
  None of 40 with either of emscripten's allocators, and none of 40 when the memory was 1 GB from the start, so it never grew.
  A probe that read `memory.size` before every access hid it as well: none of 52 runs.

Not the version: the vendored one is v3.5.3, emscripten's port v3.4.1.
The configuration differs; emscripten builds its port (tools/system_libs.py, `libmimalloc`) with, among others:

- `MI_USE_BUILTIN_THREAD_POINTER`: the thread id from `__builtin_thread_pointer()`;
- `MI_DEFAULT_ARENA_RESERVE=65536`: arenas reserved in 64 MiB chunks;
- `MI_ARENA_SLICE_SHIFT=(12+MI_SIZE_SHIFT)`: 16 KiB slices on wasm32;
- `MI_LIBC_MUSL`, `MI_MAX_ALIGN_SIZE=8`, `MI_ENABLE_LARGE_PAGES=0`;
- emmalloc as the system allocator underneath, through `sbrk`.

The vendored build sets none of them (extern/mimalloc/CMakeLists.txt builds it as on any platform).
Hypotheses, each to test on its own:

- Without the builtin thread pointer, threads may share an id, so two threads use one heap; that would fault, and only under threads.
- The default arena reservation, address space natively, is real memory on wasm, which never shrinks; that would blow up the memory.
- A newer mimalloc may fix either, so try the current release first.

To settle it: build the vendored mimalloc with emscripten's flags, one at a time, and rerun both workloads.
Until then, a threaded wasm build should configure SC_MIMALLOC off and link emscripten's port.
