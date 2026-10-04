#pragma once

#include <clean-simd/kernel.hh>

/// Runtime dispatch: one algorithm, templated on the kernel, compiled once per kernel and picked per CPU.
///
/// ```cpp
/// // bvh8-query.hh — an ordinary header; its signature must not depend on K
/// template <class K> int query_impl(bvh8 const& b, box q, i32* out, int cap) { … cimd::f32x8<K> … }
/// CIMD_DISPATCH_DECLARE(bvh8_query, query_impl);
///
/// // a call site anywhere
/// auto const hits = CIMD_DISPATCH(bvh8_query)(b, q, out, cap);
/// ```
///
/// ```cmake
/// cimd_dispatch(my-target NAME bvh8_query HEADER bvh8-query.hh FUNCTION query_impl LINK clean-simd)
/// ```
///
/// Kernels at or below the build's floor are instantiated in the target itself.
/// Each kernel above it (SC_SIMD_KERNELS) gets a generated TU compiled with that kernel's `-march`, in a static library
/// linked behind the target, so the floor's copy of every shared inline function is the one the linker keeps.
/// libs/base/clean-simd/docs/design.md has why, and what the link-map check guards.
///
/// **A dispatched header may include anything the target can, but must not define a non-template inline function
/// that only it uses**: compiled with the kernel's flags, that function would be the only copy, and any floor code
/// calling it would run the kernel's instructions.

namespace cimd
{
/// Whether this CPU and OS can run kernel `id`'s code: x86-64-v2 for sse42, v3 for avx2, v4 for avx512.
[[nodiscard]] bool cpu_supports(kernel_id id);

/// Pins every CIMD_DISPATCH on the calling thread to `id`, for tests and benchmarks that compare kernels on one machine.
/// A kernel the dispatch table lacks, or the CPU cannot run, is ignored and the best one is used instead.
/// Per thread because a test runner runs tests side by side; work the thread hands to a pool is not pinned.
void force_kernel(kernel_id id);

/// Ends a force_kernel on the calling thread.
void reset_forced_kernel();

} // namespace cimd

/// force_kernel for a scope.
struct cimd::scoped_forced_kernel
{
    explicit scoped_forced_kernel(kernel_id id) { force_kernel(id); }
    ~scoped_forced_kernel() { reset_forced_kernel(); }
    scoped_forced_kernel(scoped_forced_kernel const&) = delete;
    scoped_forced_kernel& operator=(scoped_forced_kernel const&) = delete;
};

namespace cimd::impl
{
template <class Fn>
struct dispatch_entry
{
    kernel_id id;
    Fn fn;
};

/// The calling thread's forced kernel, or false when none is.
[[nodiscard]] bool forced_kernel(kernel_id& out);

/// The last entry the CPU runs; tables list kernels from weakest to strongest, scalar first, so there is always one.
/// Each dispatched entry caches this in a static of its own — a cache in here would be shared by every entry of one
/// signature.
template <class Fn, int N>
[[nodiscard]] Fn best_entry(dispatch_entry<Fn> const (&table)[N])
{
    auto fn = table[0].fn;
    for (auto const& e : table)
        if (cpu_supports(e.id))
            fn = e.fn;
    return fn;
}

/// The calling thread's forced entry, if the table has it and the CPU runs it, else `best`.
template <class Fn, int N>
[[nodiscard]] Fn dispatch_select(dispatch_entry<Fn> const (&table)[N], Fn best)
{
    auto forced = kernel_id::scalar;
    if (forced_kernel(forced))
        for (auto const& e : table)
            if (e.id == forced && cpu_supports(e.id))
                return e.fn;
    return best;
}
} // namespace cimd::impl

/// Declares the dispatched entry `name`, a function returning a pointer to `fn<K>` for the kernel this CPU runs best.
/// cimd_dispatch() in CMake defines it.
#define CIMD_DISPATCH_DECLARE(name, fn) decltype(&fn<::cimd::scalar>) cimd_dispatch_##name()

/// The best kernel's instantiation of the function declared as `name`, to be called right away.
#define CIMD_DISPATCH(name) cimd_dispatch_##name()
