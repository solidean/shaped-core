#pragma once

#include <clean-core/thread/atomic.hh>
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
/// auto const ran = CIMD_DISPATCH_KERNEL(bvh8_query); // cimd::kernel_id::avx2
/// ```
///
/// ```cmake
/// cimd_dispatch(my-target NAME bvh8_query HEADER bvh8-query.hh FUNCTION query_impl LINK clean-simd)
/// cimd_check_link_map(my-executable)
/// ```
///
/// Kernels at or below the build's floor are instantiated in the target itself.
/// Each kernel above it (SC_SIMD_KERNELS) gets a generated TU compiled with that kernel's `-march`, in a static library
/// linked behind the target.
/// On COFF the floor's copy of every shared inline function is then the one the linker keeps; on ELF that is likely
/// rather than certain, and cimd_check_link_map on each final executable is what checks it.
/// libs/base/clean-simd/docs/design.md has the measurements.
///
/// **A dispatched header must not define a non-template inline function that only kernel code reaches.**
/// The floor's dispatch TU always instantiates `fn<scalar>`, so a helper floor code reaches has a floor copy, and that
/// copy wins.
/// A helper reached only from kernel code has a single, kernel-compiled copy: harmless at run time, since only that
/// kernel calls it, but indistinguishable in the link map from a kernel copy that won, so the check fails the build.
///
/// The first call of each entry resolves it and logs the kernel at info, in domain `cimd`.

namespace cimd
{
/// Whether this CPU and OS can run kernel `id`'s code: x86-64-v2 for sse42, v3 for avx2, v4 for avx512.
[[nodiscard]] bool cpu_supports(kernel_id id);

/// Pins every CIMD_DISPATCH on the calling thread to `id`, for tests and benchmarks that compare kernels on one machine.
/// A kernel the dispatch table lacks, or the CPU cannot run, is ignored and the best one is used instead, logged at
/// debug.
/// Per thread because a test runner runs tests side by side; work the thread hands to a pool is not pinned.
/// A thread that exits while forced leaves every thread's dispatch on the slower, TLS-reading path.
void force_kernel(kernel_id id);

/// Ends a force_kernel on the calling thread.
void reset_forced_kernel();

} // namespace cimd

/// force_kernel for a scope; nests, restoring whatever force was active before it.
struct cimd::scoped_forced_kernel
{
    explicit scoped_forced_kernel(kernel_id id);
    ~scoped_forced_kernel();
    scoped_forced_kernel(scoped_forced_kernel const&) = delete;
    scoped_forced_kernel& operator=(scoped_forced_kernel const&) = delete;

private:
    kernel_id _previous = kernel_id::scalar;
    bool _had_previous = false;
};

namespace cimd::impl
{
template <class Fn>
struct dispatch_entry
{
    kernel_id id;
    Fn fn;
};

/// How many threads have a force_kernel active; while it is zero, a dispatch reads no TLS and calls nothing.
extern cc::atomic<int> g_active_forces;

/// The calling thread's forced kernel, or false when none is.
[[nodiscard]] bool forced_kernel(kernel_id& out);

void note_resolved(char const* name, kernel_id id);
void note_unhonoured_force(char const* name, kernel_id forced, kernel_id used);

/// The last row the CPU runs; tables list kernels from weakest to strongest, scalar first, so there is always one.
/// Each dispatched entry caches this in a static of its own, since a cache in here would be shared by every entry of
/// one signature.
template <class Fn, int N>
[[nodiscard]] dispatch_entry<Fn> const& best_entry(dispatch_entry<Fn> const (&table)[N], char const* name)
{
    auto best = &table[0];
    for (auto const& e : table)
        if (cpu_supports(e.id))
            best = &e;
    note_resolved(name, best->id);
    return *best;
}

/// The calling thread's forced row, if the table has it and the CPU runs it, else `best`.
template <class Fn, int N>
[[nodiscard]] dispatch_entry<Fn> const& dispatch_select(dispatch_entry<Fn> const (&table)[N],
                                                        dispatch_entry<Fn> const& best,
                                                        char const* name)
{
    if (g_active_forces.load(cc::memory_order_relaxed) == 0)
        return best;
    auto forced = kernel_id::scalar;
    if (!forced_kernel(forced))
        return best;
    for (auto const& e : table)
        if (e.id == forced && cpu_supports(e.id))
            return e;
    note_unhonoured_force(name, forced, best.id);
    return best;
}
} // namespace cimd::impl

/// Declares the dispatched entry `name`: CIMD_DISPATCH(name) is `fn<K>` for the kernel this thread runs, and
/// CIMD_DISPATCH_KERNEL(name) is that K's id.
/// Must sit at global scope: cimd_dispatch() in CMake defines the entry there.
#define CIMD_DISPATCH_DECLARE(name, fn)              \
    ::cimd::kernel_id cimd_dispatch_kernel_##name(); \
    decltype(&fn<::cimd::scalar>) cimd_dispatch_##name()

/// The instantiation of the function declared as `name` for the kernel this thread runs, to be called right away.
/// The best one the CPU runs, unless a force_kernel on this thread names another the table holds and the CPU runs.
#define CIMD_DISPATCH(name) cimd_dispatch_##name()

/// The kernel the next CIMD_DISPATCH(name) on this thread runs, forced or not.
#define CIMD_DISPATCH_KERNEL(name) cimd_dispatch_kernel_##name()
