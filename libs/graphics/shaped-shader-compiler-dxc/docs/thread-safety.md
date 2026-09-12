# Thread safety: an open finding

**Every entry into DXC is serialized behind one process-wide lock today.**
That is a holding position, not the answer, and this file is what it is holding the place for.

## What was observed

The repo's first ThreadSanitizer run (the `sanitize-thread-*` presets) reported a data race inside the vendored `libdxcompiler.so`, reproducible from `shaped-shader-compiler-dxc-test`:

- one thread allocates through DXC's own `WideCharToMultiByte` shim (`calloc`, inside libdxcompiler),
- another frees that same block on its way out of `IDxcCompiler3::Compile`.

Both threads were doing exactly what [compiler.hh](../src/shaped-shader-compiler-dxc/compiler.hh) prescribes: **a separate `ssc::dxc::compiler` instance each, on its own thread.**
So the state involved sits *below* the instance, and the per-instance rule does not cover it.

That matters beyond the test.
[shader_cache.hh](../src/shaped-shader-compiler-dxc/shader_cache.hh) is built on the per-instance rule — a thread-local compiler per worker — and compiles on several workers at once.

## What is still unknown

Nobody has established which of these it is, and the two call for different fixes:

- **Benign.** DXC's shim may hand the block over with synchronization TSan cannot see — an uninstrumented library's release/acquire looks like plain memory to the tool.
  Then nothing is wrong and the lock is pure lost throughput.
- **Real.** The block is genuinely shared with no edge, in which case concurrent compilation has been corrupting DXC's heap all along, rarely enough that nothing noticed.

The way to tell them apart is to read what libdxcompiler actually does around that shim, not to run the test more times: a race this narrow reproduces by luck.

## The lock

`SSC_DXC_SERIALIZE_INVOCATIONS` in [compiler.cc](../src/shaped-shader-compiler-dxc/compiler.cc), default `1`.
It puts `compiler::create()`, `preprocess()`, `compile()` and `~compiler()` behind one `cc::mutex`, so only one thread is inside libdxcompiler at a time.

**The destructor is on that list because releasing a COM pointer is a call into the library.**
`~compiler` releases `IDxcUtils` and `IDxcCompiler3`, and the last release of a blob frees memory libdxcompiler allocated — which is the half of the observed race that is a free.
The same reasoning is what puts the whole of `preprocess` and `compile` under the lock rather than the `Compile` call alone.
`GetOutput`, `IDxcUtils::CreateReflection` and every `ComPtr` destructor on the way out are entries too.

**Set it to `0` to get the un-serialized behaviour back** — which is what the investigation above needs, and the reason it is a define rather than a quietly-added lock.

**Take the `libdxcompiler.so` entry out of [tools/cmake/tsan-suppressions.txt](../../../../tools/cmake/tsan-suppressions.txt) as well**, or the un-serialized run reports nothing.
`dev.py` applies that list to every sanitized test run, and `called_from_lib` drops the report silently rather than counting it somewhere visible.
The investigation then reads as a clean run when it has only been muted.

The cost is real: shader compilation is the most expensive thing this library does, and the cache in front of it exists precisely because of that.
Serializing removes the parallelism `slib`'s async compilation was built for, and leaves the cache's concurrency buying only its hits.

## What a real fix probably looks like

If the race turns out to be real, a mutex on every call is the wrong shape for it.
DXC would become a **`cc::threaded_actor`** that owns the compiler and takes compile requests as messages.
That is serialization by ownership rather than by lock, which is how the rest of shaped-core serializes a resource that cannot be shared.
Callers already reach it through an async cache, so they would see a `cc::async<...>` either way.
The actor would keep the call sites unchanged while making "one thread is inside DXC" a structural property instead of a discipline.
