// Node-allocation DESIGN benchmark: the fast-path variants, side by side.
//
// Each variant is a self-contained mini-slab-allocator implemented inline below, with only the hot path
// real; refill/drain-underflow routes through bench_design::cold_refill (an opaque call in another TU that
// never fires in the timed loop). The point is to compare the *instruction mix* of each lock-removal
// strategy on whatever machine you run it on — the whole reason to ship all the code (see
// libs/base/clean-core/docs/systems/node-allocation.md).
//
// The cold refill is out-of-TU on purpose: it makes the optimizer reload the slab base on every allocation
// (it cannot prove the base survives the call), exactly as the real cc::node_allocator must (its cold path
// allocate_node_bytes_non_fast is likewise opaque). Without it the variants hoist a single fixed slab base
// into a register and report a number the real allocator can't reach; with it, the shipped `node` line below
// tracks the step2_tls_diff variant it implements. Never remove it for a "cleaner" inline stub.
//
// Variants:
//   atomic          current design: one bitmap + next, `lock and` to allocate, `lock or` to free (2 locks)
//   single          same design, no atomics at all (the single-threaded floor)
//   step1_same      local + remote bitmap in one cache line; non-atomic alloc, atomic free into remote,
//                   drain remote->local when local empties (1 lock + amortized drain)
//   step1_diff      as step1 but the remote bitmap sits in a 2nd cache line (avoids false sharing under
//                   contention, at the cost of a 2nd line touched here)
//   step2_tls_same  as step1 plus an owner check on free: owner frees non-atomically into local, only
//                   remote threads pay the atomic. Owner token = a thread_local int16 (0 locks single-thread)
//   step2_tls_diff  step2 with the remote bitmap in a 2nd cache line
//   step2_teb_same  step2 with the owner token = the TEB self pointer (one gs-relative load, no TLS-index chain)
//   step2_teb_diff  step2_teb with the remote bitmap in a 2nd cache line
//   mimalloc        cc::default_memory_resource, same batch pattern
//   system          cc::system_memory_resource (platform malloc), same batch pattern
//   node            the REAL shipped cc::node_allocator (not a mock) -- the line to trust for actual perf
//
// Workload: allocate a fixed batch of N, free all N in a fixed permuted order, repeat; 3 runs. Metric is
// millions of alloc+free pairs/s AND GB/s (= pairs/s * size). Machine-readable rows are printed as
//   RESULT,<variant>,<size>,<run>,<mops>,<gbps>
// for scripts/plot-node-allocation-design.py to parse into SVGs.

#include "bench_util.hh"
#include "node-allocation-design-refill.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <atomic>
#include <chrono>
#include <cstdio>

#if defined(_WIN32)
#include <intrin.h> // __readgsqword
#endif

using namespace cc::primitive_defines;

namespace
{
using clock = std::chrono::steady_clock;

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
cc::byte* alloc_slab(isize size)
{
    cc::byte* p = nullptr;
    cc::default_memory_resource->allocate_bytes(&p, size, size, size, cc::default_memory_resource->userdata);
    return p;
}
void free_slab(cc::byte* p, isize size)
{
    cc::default_memory_resource->deallocate_bytes(p, size, size, cc::default_memory_resource->userdata);
}

// per-thread owner tokens for the step2 variants
thread_local cc::i16 g_tls_owner = 0;
CC_FORCE_INLINE u64 tls_token()
{
    return u64(cc::u16(g_tls_owner));
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
    cc::byte* base = nullptr;

    void hydrate()
    {
        base = alloc_slab(SLAB);
        *reinterpret_cast<u64*>(base) = ~u64(0); // all 64 slots free
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE cc::byte* alloc()
    {
        cc::byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = std::atomic_ref<u64>(*reinterpret_cast<u64*>(b)).load(std::memory_order_relaxed);
        if (v == 0) [[unlikely]] // opaque refill (never hit: batch fits the slab) -- defeats base-hoisting
        {
            b = bench_design::cold_refill(b);
            base = b;
            v = *reinterpret_cast<u64*>(b);
        }
        int const slot = cc::count_trailing_zeroes(v);
        std::atomic_ref<u64>(*reinterpret_cast<u64*>(b)).fetch_and(~(u64(1) << slot), std::memory_order_relaxed); // lock and
        return b + DATA_OFF + (isize(slot) << LOG);
    }
    CC_FORCE_INLINE void free(cc::byte* p)
    {
        auto* b = reinterpret_cast<cc::byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        std::atomic_ref<u64>(*reinterpret_cast<u64*>(b)).fetch_or(u64(1) << slot, std::memory_order_relaxed); // lock or
    }
};

template <isize Size>
struct VarSingle
{
    static constexpr isize DATA_OFF = 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    cc::byte* base = nullptr;

    void hydrate()
    {
        base = alloc_slab(SLAB);
        *reinterpret_cast<u64*>(base) = ~u64(0);
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE cc::byte* alloc()
    {
        cc::byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
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
    CC_FORCE_INLINE void free(cc::byte* p)
    {
        auto* b = reinterpret_cast<cc::byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        *reinterpret_cast<u64*>(b) |= u64(1) << slot; // plain or
    }
};

// Step 1: split local/remote bitmap. Non-atomic allocate from local; atomic free into remote; drain
// remote->local when local empties. `Diff` places the remote bitmap on the 2nd cache line.
template <isize Size, bool Diff>
struct VarStep1
{
    static constexpr isize REMOTE_OFF = Diff ? 64 : 8;
    static constexpr isize DATA_OFF = Diff ? 128 : 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    cc::byte* base = nullptr;

    static CC_FORCE_INLINE u64& local(cc::byte* b) { return *reinterpret_cast<u64*>(b); }
    static CC_FORCE_INLINE u64& remote(cc::byte* b) { return *reinterpret_cast<u64*>(b + REMOTE_OFF); }

    void hydrate()
    {
        base = alloc_slab(SLAB);
        local(base) = ~u64(0);
        remote(base) = 0;
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE cc::byte* alloc()
    {
        cc::byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = local(b);   // plain load
        if (v == 0)         // local empty: drain remote (one atomic), else opaque refill (reloads base per alloc)
        {
            v = std::atomic_ref<u64>(remote(b)).exchange(0, std::memory_order_relaxed);
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
    CC_FORCE_INLINE void free(cc::byte* p)
    {
        auto* b = reinterpret_cast<cc::byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        std::atomic_ref<u64>(remote(b)).fetch_or(u64(1) << slot, std::memory_order_relaxed); // lock or (always)
    }
};

// Step 2: step 1 plus an owner check on free. The owning thread frees non-atomically into local; only a
// genuinely remote thread pays the atomic into remote. `Teb` selects the owner-token source.
template <isize Size, bool Diff, bool Teb>
struct VarStep2
{
    static constexpr isize REMOTE_OFF = Diff ? 64 : 8;
    static constexpr isize OWNER_OFF = 16;
    static constexpr isize DATA_OFF = Diff ? 128 : 64;
    static constexpr isize SLAB = next_pow2(DATA_OFF + 64 * Size);
    static constexpr isize MASK = SLAB - 1;
    static constexpr int LOG = log2i(Size);
    cc::byte* base = nullptr;

    static CC_FORCE_INLINE u64& local(cc::byte* b) { return *reinterpret_cast<u64*>(b); }
    static CC_FORCE_INLINE u64& remote(cc::byte* b) { return *reinterpret_cast<u64*>(b + REMOTE_OFF); }
    static CC_FORCE_INLINE u64& owner(cc::byte* b) { return *reinterpret_cast<u64*>(b + OWNER_OFF); }
    static CC_FORCE_INLINE u64 my_token() { return Teb ? teb_token() : tls_token(); }

    void hydrate()
    {
        base = alloc_slab(SLAB);
        local(base) = ~u64(0);
        remote(base) = 0;
        owner(base) = my_token(); // hydrated by the owning thread
    }
    void teardown() { free_slab(base, SLAB); }

    CC_FORCE_INLINE cc::byte* alloc()
    {
        cc::byte* b = base; // reload the current slab once per alloc (mirrors the real slab_base[idx] load)
        u64 v = local(b);
        if (v == 0) // local empty: drain remote, else opaque refill (reloads base per alloc)
        {
            v = std::atomic_ref<u64>(remote(b)).exchange(0, std::memory_order_relaxed);
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
    CC_FORCE_INLINE void free(cc::byte* p)
    {
        auto* b = reinterpret_cast<cc::byte*>(reinterpret_cast<u64>(p) & ~u64(MASK));
        int const slot = int((p - b - DATA_OFF) >> LOG);
        u64 const bit = u64(1) << slot;
        if (owner(b) == my_token()) // owner: non-atomic local free (predicted-taken single-thread)
            local(b) |= bit;
        else
            std::atomic_ref<u64>(remote(b)).fetch_or(bit, std::memory_order_relaxed);
    }
};

// mimalloc / system: the same batch pattern through a cc::memory_resource, for reference.
template <isize Size>
struct VarResource
{
    cc::memory_resource const* res = nullptr;
    void hydrate() {}
    void teardown() {}
    CC_FORCE_INLINE cc::byte* alloc()
    {
        cc::byte* p = nullptr;
        res->allocate_bytes(&p, Size, Size, 8, res->userdata);
        return p;
    }
    CC_FORCE_INLINE void free(cc::byte* p) { res->deallocate_bytes(p, Size, 8, res->userdata); }
};

// node: the REAL shipped cc::node_allocator, not an inline mock. Same batch pattern, so it should track the
// design variant it implements (step2_tls_diff) up to the cost of its extra not-taken branches (large-node
// check, cold-path fallback). This is the line to trust for "what does the actual allocator do here".
// Size is a power-of-two class size, so the class index is exactly log2(Size); size/align args are ignored
// for small classes (only idx matters), so the hot path is identical to allocate_node_bytes' fast path.
template <isize Size>
struct VarNode
{
    static constexpr cc::node_class_index IDX = cc::node_class_index(log2i(Size));
    cc::node_allocator* na = nullptr;
    void hydrate() { na = &cc::default_node_allocator(); }
    void teardown() {}
    CC_FORCE_INLINE cc::byte* alloc() { return na->allocate_node_bytes(IDX, Size, 8); }
    CC_FORCE_INLINE void free(cc::byte* p) { cc::node_allocation_free(p, IDX); }
};

// --- harness --------------------------------------------------------------------------------------------

template <class Var>
double one_run(Var& v, u64& acc)
{
    cc::byte* nodes[batch_n] = {};
    auto const t0 = clock::now();
    for (isize it = 0; it < iters; ++it)
    {
        for (isize i = 0; i < batch_n; ++i)
            nodes[i] = v.alloc();
        for (isize i = 0; i < batch_n; ++i)
        {
            cc::byte* const p = nodes[free_order[i]];
            acc ^= reinterpret_cast<u64>(p);
            v.free(p);
        }
    }
    double const seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return double(iters * batch_n) / seconds / 1e6; // M pairs/s
}

template <class Var>
void measure(char const* name, isize size, Var& v)
{
    v.hydrate();

    // warmup: reach steady state (local/remote migration) and warm caches
    cc::byte* nodes[batch_n] = {};
    u64 acc = 0;
    for (int w = 0; w < warmup_iters; ++w)
    {
        for (isize i = 0; i < batch_n; ++i)
            nodes[i] = v.alloc();
        for (isize i = 0; i < batch_n; ++i)
            v.free(nodes[free_order[i]]);
    }

    double best = 0;
    for (int r = 0; r < runs; ++r)
    {
        double const mops = one_run(v, acc);
        double const gbps = mops * double(size) / 1000.0; // pairs/s * bytes -> GB/s
        best = mops > best ? mops : best;
        std::printf("RESULT,%s,%lld,%d,%.2f,%.4f\n", name, (long long)size, r, mops, gbps);
    }
    std::printf("  %-16s %5lld B : %7.1f M pairs/s   %7.2f GB/s\n", name, (long long)size, best,
                best * double(size) / 1000.0);
    bench::sink ^= acc;

    v.teardown();
}

// Non-inlined, uniquely-named single-batch hot-loop probe, instantiated for the design mock and the real
// allocator below. Identical loop structure differing only in the Var type, so `dev.py assembly show` lands
// on each hot path and they can be diffed instruction-for-instruction — the check that the shipped
// allocator actually compiles to the step2_tls_diff design it was chosen from (not just claims to).
// Kept alive by references from the TEST (TU-local + noinline would otherwise be dead-code-eliminated).
template <class Var>
CC_DONT_INLINE u64 design_hotloop_probe(Var& v, cc::byte** nodes, int const* free_perm)
{
    for (isize i = 0; i < batch_n; ++i)
        nodes[i] = v.alloc();
    u64 acc = 0;
    for (isize i = 0; i < batch_n; ++i)
    {
        cc::byte* const p = nodes[free_perm[i]];
        acc ^= reinterpret_cast<u64>(p);
        v.free(p);
    }
    return acc;
}

template <isize Size>
void sweep()
{
    std::printf("\n--- size %lld B (slab metadata in %s) ---\n", (long long)Size,
                "cache line 0; diff variants use line 1 for remote");
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
        VarResource<Size> v{cc::default_memory_resource};
        measure("mimalloc", Size, v);
    }
    {
        VarResource<Size> v{&cc::system_memory_resource};
        measure("system", Size, v);
    }
    {
        VarNode<Size> v;
        measure("node", Size, v);
    }
}
} // namespace

// Full design sweep across the small size classes. Manual: analyzed via scripts/plot-node-allocation-design.py.
TEST("bench-node-design (fast-path variants)", nx::config::manual)
{
    g_tls_owner = 0x51D2; // any nonzero per-thread id for the step2_tls owner token

    std::printf("\n=== node-allocation design benchmark: alloc %lld / free %lld (permuted), x%lld iters, %d runs ===\n",
                (long long)batch_n, (long long)batch_n, (long long)iters, runs);
    std::printf("RESULT,variant,size,run,mops,gbps\n"); // machine-readable header

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
        cc::byte* probe_nodes[batch_n] = {};
        VarStep2<16, true, false> vm;
        vm.hydrate();
        bench::sink ^= design_hotloop_probe(vm, probe_nodes, free_order);
        vm.teardown();

        VarNode<16> vn;
        vn.hydrate();
        bench::sink ^= design_hotloop_probe(vn, probe_nodes, free_order);
        vn.teardown();
    }

    std::fflush(stdout);
}
