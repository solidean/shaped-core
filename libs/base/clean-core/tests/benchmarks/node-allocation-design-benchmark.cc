// Node-allocation DESIGN benchmark: the fast-path variants, side by side.
//
// Each variant is a self-contained mini-slab-allocator implemented inline below, with only the hot path real; refill and drain-underflow route through bench_design::cold_refill.
// The point is to compare the *instruction mix* of each lock-removal strategy on whatever machine you run it on, which is the whole reason to ship all the code.
// See libs/base/clean-core/docs/systems/node-allocation.md.
//
// cold_refill is an opaque out-of-TU call on purpose, and node-allocation-design-refill.hh says why.
// With it, the shipped `node` line below tracks the step2_tls_diff variant it implements.
// Never replace it with a "cleaner" inline stub.
//
// Variants:
//   atomic          current design: one bitmap + next, `lock and` to allocate, `lock or` to free (2 locks)
//   single          same design, no atomics at all (the single-threaded floor)
//   step1_same      local + remote bitmap in one cache line; non-atomic alloc, atomic free into remote,
//                   drain remote->local when local empties (1 lock + amortized drain)
//   step1_diff      as step1 but the remote bitmap sits in a 2nd cache line (avoids false sharing under
//                   contention, at the cost of a 2nd line touched here)
//   step2_tls_same  as step1 plus an owner check on free: the owner frees non-atomically into local, and
//                   only remote threads pay the atomic; owner token = a thread_local int16 (0 locks single-thread)
//   step2_tls_diff  step2 with the remote bitmap in a 2nd cache line
//   step2_teb_same  step2 with the owner token = the TEB self pointer (one gs-relative load, no TLS-index chain)
//   step2_teb_diff  step2_teb with the remote bitmap in a 2nd cache line
//   mimalloc        cc::default_memory_resource, same batch pattern
//   system          cc::system_memory_resource (platform malloc), same batch pattern
//   node            the REAL shipped cc::node_allocator (not a mock) -- the line to trust for actual perf
//
// Workload: allocate a fixed batch of N, free all N in a fixed permuted order, repeat; 3 runs.
// Metric is millions of alloc+free pairs/s AND GB/s (= pairs/s * size).
// Machine-readable rows are printed as
//   RESULT,<variant>,<size>,<run>,<mops>,<gbps>
// for scripts/plot-node-allocation-design.py to parse into SVGs.
//
// OFF IN THE TREE.
// Flip the flag below to 1 to rerun the experiment.
// This is a settled design study rather than a regression gate.
// It sweeps 9 size classes across 10 variant templates, which costs ~9 s to compile — 2.2x the next-worst TU in the repo, paid on every build of every preset.
// Being off means it rots, and that is the accepted trade.
// If the surrounding code has drifted far enough to break this file, the conclusions it reached are due a rerun anyway.
// docs/notes/build-times.md records the measurement behind the decision.
#define CC_BENCH_NODE_ALLOCATION_DESIGN 0

#if CC_BENCH_NODE_ALLOCATION_DESIGN

#include "bench_util.hh"
#include "node-allocation-design-refill.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

#include <cstdio>

#if defined(_WIN32)
#include <intrin.h> // __readgsqword
#endif

using namespace cc::primitive_defines;

namespace
{

constexpr isize batch_n = 10;      // nodes allocated/freed per iteration (well within one slab)
constexpr isize iters = 1'000'000; // iterations per timed run
constexpr int runs = 3;            // repeated timed runs
constexpr int warmup_iters = 4000; // untimed iterations to reach steady state / warm caches
constexpr int free_order[batch_n] = {4, 0, 8, 2, 6, 9, 1, 5, 3, 7}; // fixed non-sequential free order

// small compile-time helpers (Size is always a power of two here)
constexpr int log2i(isize v)
{
    int r = 0;
    while (v > 1)
    {
        v >>= 1;
        ++r;
    }
    return r;
}
constexpr isize next_pow2(isize v)
{
    isize p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

// aligned slab storage via mimalloc (alignment == size, so ptr & ~(size-1) recovers the base)
byte* alloc_slab(isize size)
{
    byte* p = nullptr;
    cc::default_memory_resource->allocate_bytes(&p, size, size, size, cc::default_memory_resource->userdata);
    return p;
}
void free_slab(byte* p, isize size)
{
    cc::default_memory_resource->deallocate_bytes(p, size, size, cc::default_memory_resource->userdata);
}

// per-thread owner tokens for the step2 variants
thread_local i16 g_tls_owner = 0;
CC_FORCE_INLINE u64 tls_token()
{
    return u64(u16(g_tls_owner));
}
CC_FORCE_INLINE u64 teb_token()
{
#if defined(CC_OS_WINDOWS) && defined(CC_ARCH_X64)
    return __readgsqword(0x30); // TEB self-pointer: one fixed-offset gs load, no TLS-index indirection
#elif defined(CC_OS_WINDOWS) && defined(CC_ARCH_ARM64)
    return __readx18qword(0x30); // TEB self-pointer: x18 holds the TEB on ARM64 Windows (same NtTib.Self offset)
#else
    return reinterpret_cast<u64>(&g_tls_owner); // portable stand-in: a unique per-thread address
#endif
}

// --- variant implementations ----------------------------------------------------------------------------
//
// Layout convention: local bitmap at slab offset 0. Same-cache-line variants keep the remote bitmap at
// offset 8 and the owner at 16; diff-cache-line variants put the remote bitmap at offset 64 (the 2nd line).
// The node data region starts past the metadata (DATA_OFF), and slot i lives at base+DATA_OFF+i*Size, so
// `slot = (ptr - base - DATA_OFF) >> log2(Size)` and `base = ptr & ~(SLAB-1)`. We use a full 64-slot bitmap.

template <isize Size>
struct VarAtomic
{
    static constexpr isize DATA_OFF = 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    byte* base = nullptr;

    void hydrate()
    {
        base = alloc_slab(SLAB);
        *reinterpret_cast<u64*>(base) = ~u64(0); // all 64 slots free
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE byte* alloc()
    {
        byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = cc::atomic_ref<u64>(*reinterpret_cast<u64*>(b)).load(cc::memory_order_relaxed);
        if (v == 0) [[unlikely]] // opaque refill (never hit: batch fits the slab) -- defeats base-hoisting
        {
            b = bench_design::cold_refill(b);
            base = b;
            v = *reinterpret_cast<u64*>(b);
        }
        int const slot = cc::count_trailing_zeroes(v);
        cc::atomic_ref<u64>(*reinterpret_cast<u64*>(b)).fetch_and(~(u64(1) << slot), cc::memory_order_relaxed); // lock and
        return b + DATA_OFF + (isize(slot) << LOG);
    }
    CC_FORCE_INLINE void free(byte* p)
    {
        auto* b = reinterpret_cast<byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        cc::atomic_ref<u64>(*reinterpret_cast<u64*>(b)).fetch_or(u64(1) << slot, cc::memory_order_relaxed); // lock or
    }
};

template <isize Size>
struct VarSingle
{
    static constexpr isize DATA_OFF = 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    byte* base = nullptr;

    void hydrate()
    {
        base = alloc_slab(SLAB);
        *reinterpret_cast<u64*>(base) = ~u64(0);
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE byte* alloc()
    {
        byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = *reinterpret_cast<u64*>(b);
        if (v == 0) [[unlikely]] // opaque refill -- defeats base-hoisting so base is reloaded per alloc
        {
            b = bench_design::cold_refill(b);
            base = b;
            v = *reinterpret_cast<u64*>(b);
        }
        int const slot = cc::count_trailing_zeroes(v);
        *reinterpret_cast<u64*>(b) = v & ~(u64(1) << slot); // plain and
        return b + DATA_OFF + (isize(slot) << LOG);
    }
    CC_FORCE_INLINE void free(byte* p)
    {
        auto* b = reinterpret_cast<byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        *reinterpret_cast<u64*>(b) |= u64(1) << slot; // plain or
    }
};

// Step 1: split local/remote bitmap.
// Non-atomic allocate from local, atomic free into remote, and drain remote->local when local empties.
// `Diff` places the remote bitmap on the 2nd cache line.
template <isize Size, bool Diff>
struct VarStep1
{
    static constexpr isize REMOTE_OFF = Diff ? 64 : 8;
    static constexpr isize DATA_OFF = Diff ? 128 : 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    byte* base = nullptr;

    static CC_FORCE_INLINE u64& local(byte* b) { return *reinterpret_cast<u64*>(b); }
    static CC_FORCE_INLINE u64& remote(byte* b) { return *reinterpret_cast<u64*>(b + REMOTE_OFF); }

    void hydrate()
    {
        base = alloc_slab(SLAB);
        local(base) = ~u64(0);
        remote(base) = 0;
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE byte* alloc()
    {
        byte* b = base;   // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = local(b); // plain load
        if (v == 0)       // local empty: drain remote (one atomic), else opaque refill (reloads base per alloc)
        {
            v = cc::atomic_ref<u64>(remote(b)).exchange(0, cc::memory_order_relaxed);
            if (v == 0) [[unlikely]]
            {
                b = bench_design::cold_refill(b);
                base = b;
                v = local(b);
            }
        }
        int const slot = cc::count_trailing_zeroes(v);
        local(b) = v & ~(u64(1) << slot); // plain and
        return b + DATA_OFF + (isize(slot) << LOG);
    }
    CC_FORCE_INLINE void free(byte* p)
    {
        auto* b = reinterpret_cast<byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        cc::atomic_ref<u64>(remote(b)).fetch_or(u64(1) << slot, cc::memory_order_relaxed); // lock or (always)
    }
};

// Step 2: step 1 plus an owner check on free.
// The owning thread frees non-atomically into local; only a genuinely remote thread pays the atomic into remote.
// `Teb` selects the owner-token source.
template <isize Size, bool Diff, bool Teb>
struct VarStep2
{
    static constexpr isize REMOTE_OFF = Diff ? 64 : 8;
    static constexpr isize OWNER_OFF = 16;
    static constexpr isize DATA_OFF = Diff ? 128 : 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    byte* base = nullptr;

    static CC_FORCE_INLINE u64& local(byte* b) { return *reinterpret_cast<u64*>(b); }
    static CC_FORCE_INLINE u64& remote(byte* b) { return *reinterpret_cast<u64*>(b + REMOTE_OFF); }
    static CC_FORCE_INLINE u64& owner(byte* b) { return *reinterpret_cast<u64*>(b + OWNER_OFF); }
    static CC_FORCE_INLINE u64 my_token() { return Teb ? teb_token() : tls_token(); }

    void hydrate()
    {
        base = alloc_slab(SLAB);
        local(base) = ~u64(0);
        remote(base) = 0;
        owner(base) = my_token(); // hydrated by the owning thread
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE byte* alloc()
    {
        byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = local(b);
        if (v == 0) // local empty: drain remote, else opaque refill (reloads base per alloc)
        {
            v = cc::atomic_ref<u64>(remote(b)).exchange(0, cc::memory_order_relaxed);
            if (v == 0) [[unlikely]]
            {
                b = bench_design::cold_refill(b);
                base = b;
                v = local(b);
            }
        }
        int const slot = cc::count_trailing_zeroes(v);
        local(b) = v & ~(u64(1) << slot);
        return b + DATA_OFF + (isize(slot) << LOG);
    }
    CC_FORCE_INLINE void free(byte* p)
    {
        auto* b = reinterpret_cast<byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        u64 const bit = u64(1) << slot;
        if (owner(b) == my_token()) // owner: non-atomic local free (predicted-taken single-thread)
            local(b) |= bit;
        else
            cc::atomic_ref<u64>(remote(b)).fetch_or(bit, cc::memory_order_relaxed);
    }
};

// mimalloc / system: the same batch pattern through a cc::memory_resource, for reference.
template <isize Size>
struct VarResource
{
    cc::memory_resource const* res = nullptr;
    void hydrate() {}
    void teardown() {}
    CC_FORCE_INLINE byte* alloc()
    {
        byte* p = nullptr;
        res->allocate_bytes(&p, Size, Size, 8, res->userdata);
        return p;
    }
    CC_FORCE_INLINE void free(byte* p) { res->deallocate_bytes(p, Size, 8, res->userdata); }
};

// node: the REAL shipped cc::node_allocator, not an inline mock.
// Same batch pattern, so it should track the design variant it implements (step2_tls_diff), up to the cost of its extra not-taken branches — the large-node check and the cold-path fallback.
// This is the line to trust for "what does the actual allocator do here".
// Size is a power-of-two class size, so the class index is exactly log2(Size); size/align args are ignored
// for small classes (only idx matters), so the hot path is identical to allocate_node_bytes' fast path.
template <isize Size>
struct VarNode
{
    static constexpr cc::node_class_index IDX = cc::node_class_index(log2i(Size));
    cc::node_allocator* na = nullptr;
    void hydrate() { na = &cc::default_node_allocator(); }
    void teardown() {}
    CC_FORCE_INLINE byte* alloc() { return na->allocate_node_bytes(IDX, Size, 8); }
    CC_FORCE_INLINE void free(byte* p) { cc::node_allocation_free(p, IDX); }
};

// --- harness --------------------------------------------------------------------------------------------

/// One variant at one size, as one measured loop.
///
/// The loop name is `<variant> @<size>B`, which is what scripts/plot-node-allocation-design.py splits on when it
/// reads the benchmark's JSON sidecar.
/// It used to scrape `RESULT,` CSV rows off stdout and median three timed runs itself; the sidecar carries the median
/// and its interval already, computed over hundreds of samples rather than three.
template <class Var>
void measure(char const* name, isize size, Var& v)
{
    v.hydrate();

    byte* nodes[batch_n] = {};

    nx::bench::run(cc::format("{} @{}B", name, size),
                   {.min_time_secs = 0.1, .max_samples = 512, .target_relative_error = 0.05, .no_baseline = true},
                   [&](nx::bench::iteration& it)
                   {
                       u64 acc = 0;
                       for (isize i = 0; i < batch_n; ++i)
                           nodes[i] = v.alloc();
                       for (isize i = 0; i < batch_n; ++i)
                       {
                           byte* const p = nodes[free_order[i]];
                           acc ^= reinterpret_cast<u64>(p);
                           v.free(p);
                       }
                       nx::bench::sink(acc);

                       it.items(batch_n); // alloc+free pairs
                       it.record("bytes", cc::rec::unit_bytes, f64(batch_n) * f64(size));
                   });

    v.teardown();
}

// Non-inlined, uniquely-named single-batch hot-loop probe, instantiated for the design mock and for the real allocator below.
// The loop structure is identical and differs only in the Var type, so `dev.py assembly show` lands on each hot path and they can be diffed instruction-for-instruction.
// That is the check that the shipped allocator actually compiles to the step2_tls_diff design it was chosen from, rather than only claiming to.
// Kept alive by references from the TEST (TU-local + noinline would otherwise be dead-code-eliminated).
template <class Var>
CC_DONT_INLINE u64 design_hotloop_probe(Var& v, byte** nodes, int const* free_perm)
{
    for (isize i = 0; i < batch_n; ++i)
        nodes[i] = v.alloc();
    u64 acc = 0;
    for (isize i = 0; i < batch_n; ++i)
    {
        byte* const p = nodes[free_perm[i]];
        acc ^= reinterpret_cast<u64>(p);
        v.free(p);
    }
    return acc;
}

template <isize Size>
void sweep()
{
    {
        VarAtomic<Size> v;
        measure("atomic", Size, v);
    }
    {
        VarSingle<Size> v;
        measure("single", Size, v);
    }
    {
        VarStep1<Size, false> v;
        measure("step1_same", Size, v);
    }
    {
        VarStep1<Size, true> v;
        measure("step1_diff", Size, v);
    }
    {
        VarStep2<Size, false, false> v;
        measure("step2_tls_same", Size, v);
    }
    {
        VarStep2<Size, true, false> v;
        measure("step2_tls_diff", Size, v);
    }
    {
        VarStep2<Size, false, true> v;
        measure("step2_teb_same", Size, v);
    }
    {
        VarStep2<Size, true, true> v;
        measure("step2_teb_diff", Size, v);
    }
    {
        VarResource<Size> v = {cc::default_memory_resource};
        measure("mimalloc", Size, v);
    }
    {
        VarResource<Size> v = {&cc::system_memory_resource};
        measure("system", Size, v);
    }
    {
        VarNode<Size> v;
        measure("node", Size, v);
    }
}
} // namespace

// Full design sweep across the small size classes.
// Analyzed via scripts/plot-node-allocation-design.py, which reads the --json sidecar.
BENCHMARK("bench-node-design - fast-path variants")
{
    g_tls_owner = 0x51D2; // any nonzero per-thread id for the step2_tls owner token

    sweep<1>();
    sweep<2>();
    sweep<4>();
    sweep<8>();
    sweep<16>();
    sweep<32>();
    sweep<64>();
    sweep<128>();
    sweep<256>();

    // Keep the two hot-loop probes alive as searchable disassembly symbols (not timed): the design mock
    // step2_tls_diff vs the real cc::node_allocator, both at 16 B. Compare via
    //   dev.py assembly show 'design_hotloop_probe<...VarStep2<16,true,false>...>'
    //   dev.py assembly show 'design_hotloop_probe<...VarNode<16>...>'
    {
        byte* probe_nodes[batch_n] = {};
        VarStep2<16, true, false> vm;
        vm.hydrate();
        nx::bench::sink(design_hotloop_probe(vm, probe_nodes, free_order));
        vm.teardown();

        VarNode<16> vn;
        vn.hydrate();
        nx::bench::sink(design_hotloop_probe(vn, probe_nodes, free_order));
        vn.teardown();
    }
}

#endif // CC_BENCH_NODE_ALLOCATION_DESIGN
