#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <nexus/bench/fwd.hh>

#include <type_traits>

#if defined(CC_COMPILER_MSVC)
#include <intrin.h> // _ReadWriteBarrier
#endif

// What keeps a benchmark measuring the code it names.
//
// A benchmark measures code the compiler is free to delete: nothing observes the result, so the work is dead and the
// loop reports the cost of nothing.
// The two constructs that prevent it are DIFFERENT THINGS, and conflating them is the usual reason a harness measures
// more than it meant to.
//
// `keep` and `sink` are READ guards.
// They claim one value is observed, cost nothing at run time, and constrain the optimizer no further than that.
// `compiler_barrier` is a WRITE barrier over all of memory.
// It also costs nothing at run time, and it constrains a great deal: every store before it is live, and no load after
// it may be hoisted above it.
//
// `evict_data_caches` is the odd one out, and is not an optimizer construct at all.
// It costs real milliseconds and puts the MACHINE into a cold state — see its own comment.

namespace nx::bench::impl
{
// MSVC has no inline asm on x64, so the read guard goes through an out-of-line call the optimizer cannot see into.
// Never called on clang or gcc, where the guard is an empty asm block and free.
void observe(void const volatile* p);
} // namespace nx::bench::impl

namespace nx::bench
{
/// A compile-time barrier over all of memory.
/// **Zero instructions** — nothing is flushed and no cache line moves.
///
/// What it buys is ordering against the optimizer: a store before it cannot be sunk past it, and a load after it
/// cannot be hoisted above it.
/// That is what stops a loop body from being collapsed across iterations.
///
/// Not what puts the machine in a cold state — evict_data_caches is that, and it is a run-time operation with a
/// run-time cost.
CC_FORCE_INLINE void compiler_barrier()
{
#if defined(CC_COMPILER_MSVC)
    _ReadWriteBarrier();
#else
    asm volatile("" : : : "memory");
#endif
}

/// Claims `v` is observed, and hands it back, so it wraps an expression rather than needing a named local.
///
///     auto const h = nx::bench::keep(hash(key));
///
/// **Deliberately NOT a memory barrier**, which is what makes it cheap: it claims that this one value is read and
/// nothing else, so the optimizer keeps everything feeding `v` and stays free elsewhere.
/// Reach for compiler_barrier where ordering over all of memory is what is actually wanted.
///
/// **On a container it keeps the HANDLE live, not the data.**
/// `keep(vec)` claims the pointer and size are read, and says nothing about the bytes they point at — so a loop that
/// fills a vector and keeps it can still have the stores removed.
/// `sink(cc::span<cc::byte const>)` is the guard for contents.
///
/// MSVC pays more than clang and gcc do: with no inline asm on x64 this becomes an out-of-line call plus a full
/// compiler barrier, so it is neither free nor as narrow as the comment above promises.
template <class T>
CC_FORCE_INLINE T&& keep(T&& v) noexcept
{
#if defined(CC_COMPILER_MSVC)
    impl::observe(&reinterpret_cast<byte const volatile&>(v));
    _ReadWriteBarrier();
#else
    // The constraint is split rather than written once as "r,m".
    //
    // Offered both, the compiler is free to pick "m" for anything already addressable, which means a spill to memory
    // for a value that was sitting in a register.
    // Demanding "r" where the value fits in one leaves it nothing to spill, and "m" is only reached for what could not
    // have been in a register anyway.
    if constexpr (std::is_trivially_copyable_v<std::remove_cvref_t<T>> && sizeof(v) <= sizeof(void*))
        asm volatile("" : : "r"(v));
    else
        asm volatile("" : : "m"(v));
#endif
    return cc::forward<T>(v);
}

/// The statement form of `keep`: claims `value` is observed, and returns nothing.
///
/// This is what a benchmark body writes its result into, and it replaces the file-scope `volatile` a hand-rolled
/// benchmark reaches for — a volatile costs a real store to a real address on every iteration, and this costs nothing.
template <class T>
CC_FORCE_INLINE void sink(T const& value) noexcept
{
    (void)bench::keep(value);
}

/// Claims the CONTENTS of `bytes` are observed, which the scalar overload cannot do for a buffer.
///
/// This one DOES clobber memory, because that is the only way to say "everything written into that range is read".
/// So it is the more expensive guard, and the right one exactly when the thing produced is a buffer rather than a
/// value: a decoded image, a filled vector, a serialized blob.
CC_FORCE_INLINE void sink(cc::span<byte const> bytes) noexcept
{
#if defined(CC_COMPILER_MSVC)
    impl::observe(bytes.data());
    _ReadWriteBarrier();
#else
    asm volatile("" : : "r"(bytes.data()), "r"(bytes.size()) : "memory");
#endif
}

/// How much evict_data_caches streams when it is not told.
///
/// **Provisional.** A real answer is the last-level cache size, which needs system information shaped-core does not
/// have yet; this is a constant picked to exceed the last-level cache of a typical desktop part with room to spare.
/// Replace it, not its callers, when sysinfo lands.
inline constexpr isize default_evict_bytes = isize(64) * 1024 * 1024;

/// Displaces cached data by streaming over a buffer, so the next access in a benchmark is a real miss.
///
/// **Run-time, not compile-time.** This costs milliseconds, unlike everything above it in this header.
/// Call it between `iteration::pause` and `iteration::resume` so its cost lands outside the measurement.
///
/// **It is approximate, and what it does not do matters as much as what it does.**
/// It does not evict the TLB, the branch predictors or the instruction cache, so a benchmark calling this and
/// reporting "cold" is claiming something stronger than it measured.
/// The eviction itself is a sequential read, which a hardware prefetcher services far faster than the random pattern
/// it is meant to displace, and it works better on an inclusive last-level cache than on a victim cache.
///
/// `bytes` of 0 means the whole buffer, and a negative one means no work at all rather than an error.
/// The buffer is allocated once, at the first call, at default_evict_bytes; a larger request is clamped to it rather
/// than growing it, which is a limitation of not knowing the cache size rather than a deliberate cap.
void evict_data_caches(isize bytes = 0);
} // namespace nx::bench
