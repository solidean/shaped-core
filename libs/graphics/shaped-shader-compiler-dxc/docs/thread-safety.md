# Thread safety: a closed finding

**Concurrent compilation is safe.**
One `ssc::dxc::compiler` per thread, compiling at the same time as every other thread's, corrupts nothing.
Neither does sharing one instance — though the API still asks for one each, because DXC promises nothing about sharing.

This file exists because ThreadSanitizer says otherwise, loudly, and said so for long enough that a process-wide lock was added on the strength of it.
The lock is gone.
What follows is why the reports are wrong, how that was established, and what is left.

Established against DXC v1.9.2602.24 on glibc 2.41, x86-64 Linux — the pinned release.
Re-established unchanged against v1.10.2605.37's binaries while that release was being evaluated, so the conclusions are not specific to the pin.

## The reports, and what they actually are

Two families, and neither is DXC sharing mutable state between compiles.

### glibc's locale cache — was four reports in five, now none

DXC's `ScopedLocale` (its `WinAdapter.h`) builds a `locale_t` with `newlocale(LC_CTYPE_MASK, "C.UTF-8", …)` and destroys it with `freelocale`.
It does that **around every single string conversion** `WideCharToMultiByte` and `MultiByteToWideChar` make.
Everything DXC touches there is stack-local, and one thread creates and destroys it inside one scope.

What crosses threads is glibc's own locale data, which is a refcounted process-global.
One thread's `newlocale` takes a reference on data another thread loaded; a `freelocale` elsewhere drops the last one and frees it.
glibc guards that with `__libc_setlocale_lock`, and `__freelocale` really does call `__pthread_rwlock_wrlock`.
But it reaches the rwlock through glibc's *internal* alias rather than the interposable `pthread_rwlock_wrlock`.
TSan's interceptor never fires, so no happens-before edge is recorded.
The allocation and the free are then two unordered writes to one block, which is exactly what TSan printed.

The report named `WideCharToMultiByte` as the allocating frame, which is what made this look like DXC's own heap.
It is not: the `calloc` is inside glibc's `wcstombs` loading a converter, and DXC's shim is merely the caller above it.

`ssc::dxc::pin_utf8_locale` in [compiler.cc](../src/shaped-shader-compiler-dxc/compiler.cc) removes this family at the source rather than muting it.
It takes one reference to the same locale `ScopedLocale` will ask for and never releases it, so the data stays loaded and no conversion is ever the one that loads or frees it.
That is a workaround for an upstream inefficiency — DXC should cache its own locale — and it is marked as one.

### LLVM's ManagedStatic — one report, suppressed

The one that remains, once per process:

```
Atomic read of size 1 by thread T4:     pthread_mutex_lock  <- llvm::sys::MutexImpl::acquire()
Previous write of size 8 by thread T1:  malloc              <- llvm::sys::MutexImpl::MutexImpl(bool)  (mutexes: write M0)
Location is heap block of size 40 allocated by thread T1
```

Under gdb the creating stack is `MutexImpl::MutexImpl` ← `object_creator<sys::SmartMutex<true>>` ← `ManagedStaticBase::RegisterManagedStatic`,
and above that `llvm::sys::RemoveFileOnSignal` ← `clang::CompilerInstance::createOutputFile` ← `DxcCompiler::Compile`.
So it is LLVM's lazily-created lock for the signal-handler file list, built by whichever thread compiles first.

DXC's `ManagedStatic` holds `std::atomic<void*> Ptr`, stores it with `memory_order_release` under a `std::recursive_mutex`, and loads it with `memory_order_acquire`.
That is a correct double-checked lazy init.
TSan models no atomics in an uninstrumented library, so that release/acquire edge does not exist as far as it is concerned.

`called_from_lib:libdxcompiler.so` in [tsan-suppressions.txt](../../../../tools/cmake/tsan-suppressions.txt) covers it, and after the locale fix that is the only thing it covers.

## How this was established

Reading the sources settles what the code does; the controls below settle that TSan's blindness is the whole explanation.
Each one is a few lines that link no DXC and still produce the report being explained.

**The locale family, with no DXC at all:**

```cpp
// 4 threads x 200 iterations of what ScopedLocale does per conversion.
locale_t loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
locale_t prev = uselocale(loc);
char out[64];
wcstombs(out, L"hello", sizeof(out));
uselocale(prev);
freelocale(loc);
```

That prints the same three libc frames (`__freelocale` and two unsymbolized neighbours) the DXC run does.
With `LC_ALL_MASK, "C"` it prints nothing, because glibc answers that one from a static object and never allocates.
That is why the locale *name* matters, and why `pin_utf8_locale` mirrors `ScopedLocale`'s candidate list rather than picking its own.

**The ManagedStatic family, with correct code:**

```cpp
// In a .so built WITHOUT -fsanitize=thread, called from a TSan main on 8 threads.
void* p = g_ptr.load(std::memory_order_acquire);
if (p == nullptr)
{
    std::lock_guard<std::mutex> lock(g_init);
    p = g_ptr.load(std::memory_order_relaxed);
    if (p == nullptr) { p = new_pthread_mutex(); g_ptr.store(p, std::memory_order_release); }
}
pthread_mutex_lock((pthread_mutex_t*)p);
```

TSan reports that as a race, in the same shape as the DXC one down to the 40-byte block and the `mutexes: write M0` annotation.
Provably correct code, reported, because the `.so` is uninstrumented.

**And the behaviour, not just the reasoning:**

- 12,800 concurrent compiles (16 threads x 400), once with an instance per thread and once with one shared `IDxcCompiler3`, hashing every DXIL blob.
  Zero failures, zero mismatches, one hash throughout.
- 1,440 more under AddressSanitizer: clean.
- Sharing one instance across eight threads produced no report the per-instance mode did not, so the reports never depended on the rule they appeared to contradict.

## What it was worth

| Change | Measured |
|---|---|
| Removing the process-wide lock | 12 threads x 200 compiles: 3.13 s → 0.67 s (**4.7x**) |
| `pin_utf8_locale` | 12 threads x 300 compiles: 1.03 s → 0.68 s (**34%**) |

The second is not sanitizer hygiene that happens to be free.
glibc's locale rwlock was serializing a real share of every concurrent compile, and holding one reference is what stops it.

## Windows: settled from source, not measured

Everything above was measured on Linux, because ThreadSanitizer does not run on Windows.

The one reason it might not transfer is LLVM's `PassRegistry`, which DXC locks differently per platform.
`include/llvm/PassRegistry.h` wraps its `sys::SmartRWMutex<true> Lock` in `#ifndef LLVM_ON_WIN32`, with a "HLSL Change" comment saying Windows uses a mechanism of its own instead.
[DXC #8819](https://github.com/microsoft/DirectXShaderCompiler/issues/8819) reports concurrent first calls to `IDxbcConverter::Convert` corrupting the registry's `DenseMap`.
That is `dxilconv`, which does DXBC to DXIL and which nothing here links — `ssc::dxc` uses `dxcompiler` alone.

**`dxcompiler` registers every pass at DLL load, and only reads the registry afterwards.**
Its `DllMain` runs `InitMaybeFail`, which calls `hlsl::SetupRegistryPassForHLSL` and `SetupRegistryPassForPIX` under the loader lock, before any `DxcCreateInstance`.
The Windows arm of `lib/IR/PassRegistry.cpp` states that design outright: registration is single-threaded at DllMain time, checked by an assert on the registering thread id, so reads need no lock.
That was read against v1.9.2602.24's sources; not every `registerPass` caller was traced.

The Linux side of it is measured too.
#8819 notes that identical inputs do not reproduce the race, so the check was rerun with eight threads each starting on a *different* shader.
Loops, atomics, groupshared plus barriers, texture sampling, wave intrinsics, unrolled math, structured buffers — so that every thread's first compile walks a different pass path.
Same two families, no third; clean under AddressSanitizer and un-sanitized.

## What would reopen this

- **A DXC release that changes `ScopedLocale` or `ManagedStatic`.**
  Neither changed between v1.9.2602.24, v1.9.2607, v1.10.2605.37 and `main` at the time of writing, so a bump is not expected to move any of this.
  Running this against v1.10.2605.37's `libdxcompiler.so` bore that out: same two families, and `pin_utf8_locale` still leaves only the ManagedStatic one.
- **A `dxcompiler` compile path that registers a pass after DLL load.**
  On Windows that would be an unlocked write to a registry every other compile reads, which is #8819's mechanism.
- **A TSan report through `ssc::dxc` that is not one of the two families above.**
  The suppression is scoped to `libdxcompiler.so` and to nothing of ours, so our own frames are still fully checked.
  A new report is a new finding.
- **Re-running the investigation** means taking `called_from_lib:libdxcompiler.so` out of [tsan-suppressions.txt](../../../../tools/cmake/tsan-suppressions.txt).
  Then: `uv run dev.py test shaped-shader-compiler-dxc-test --preset sanitize-thread-linux-clang --repeat 30`.
  One iteration in a handful trips the ManagedStatic report; nothing else should appear.
  `dev.py` applies that list to every sanitized run, and `called_from_lib` drops a report silently.
  Leaving it in makes the investigation read as a clean run when it has only been muted.

## Where compilation is headed: async

**`ssc::dxc` predates `cc::async`, and its compiler API is synchronous for that reason alone.**
The finding above used to gate this question; it no longer does.

The synchronous `compiler` stays the API.
[shader_cache.hh](../src/shaped-shader-compiler-dxc/shader_cache.hh) keeps compiling on several workers at once with a thread-local compiler each.
That is now known to be exactly as safe as it claimed to be.
A synchronous core is simpler to drive from tools, and nothing here argues for changing it.

**Preprocessing touches file IO** through the include resolver, and that is the part that still wants to be async.
