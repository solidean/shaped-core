#include "allocation.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/thread/atomic.hh>
#include <mimalloc-stats.h>
#include <mimalloc.h>

#if defined(__EMSCRIPTEN__)
#include <cstdlib>
#endif

using namespace cc::primitive_defines;

// mimalloc-backed implementation of cc::default_memory_resource.
// The <mimalloc.h> dependency is confined to this translation unit, so clean-core's headers stay free of it.

namespace
{
#if defined(__EMSCRIPTEN__)
/// The bytes of the live blocks emscripten's malloc holds for this resource, which mimalloc's statistics do not see.
constinit cc::atomic<i64> g_large_live = {0};

[[nodiscard]] i64 large_live_bytes()
{
    return g_large_live.load(cc::memory_order_relaxed);
}

// **On wasm a block is mimalloc's exactly while its size is at most this**, and emscripten's malloc's above it.
// wasm memory never shrinks and mimalloc never gives an arena back, so a growing vector strands an arena at every step
// until the heap is gone (bug-cc-mimalloc-wasm-arenas-exhaust-heap); emscripten's malloc merges what is freed.
// mimalloc is ahead of it up to 1 KiB there and 2-3.5x behind from 4 KiB, so the split costs no speed either.
// Every size a caller passes is the block's current size, so routing on it finds the allocator that made the block.
constexpr isize mi_max_bytes = 1024;

isize large_try_allocate_bytes(byte** out_ptr, isize min_bytes, isize alignment)
{
    void* p = nullptr;
    auto const effective_alignment = alignment < isize(sizeof(void*)) ? isize(sizeof(void*)) : alignment;
    *out_ptr = posix_memalign(&p, size_t(effective_alignment), size_t(min_bytes)) == 0 ? static_cast<byte*>(p) : nullptr;
    if (*out_ptr == nullptr)
        return -1;
    cc::large_allocations::impl::on_allocate(*out_ptr, min_bytes);
    // exactly min_bytes, so the block stays above mi_max_bytes for its whole life
    return min_bytes;
}
#else
constexpr isize mi_max_bytes = isize(1) << 62;

/// Every block is mimalloc's off wasm, so its statistics see all of them.
[[nodiscard]] i64 large_live_bytes()
{
    return 0;
}
#endif

isize mi_try_allocate_bytes(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    CC_UNUSED(userdata);

    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "alignment must be a power of 2");
    CC_ASSERT(min_bytes >= 0 && min_bytes <= max_bytes, "must have 0 <= min_bytes <= max_bytes");

    // Contract: min_bytes == 0 always sets *out_ptr to nullptr and returns 0
    if (min_bytes == 0)
    {
        *out_ptr = nullptr;
        return 0;
    }

#if defined(__EMSCRIPTEN__)
    if (min_bytes > mi_max_bytes)
    {
        auto const large = large_try_allocate_bytes(out_ptr, min_bytes, alignment);
        if (large > 0)
            g_large_live.fetch_add(large, cc::memory_order_relaxed);
        return large;
    }
#endif

    void* p = mi_malloc_aligned(static_cast<size_t>(min_bytes), static_cast<size_t>(alignment));
    *out_ptr = static_cast<byte*>(p);
    if (p == nullptr)
    {
        return -1;
    }
    // One compare on the small path; see large_allocations.hh.
    cc::large_allocations::impl::on_allocate(p, min_bytes);

    // Report mimalloc's actual usable size (size-class rounding), clamped into
    // [min_bytes, max_bytes] so containers can claim the extra capacity for free.
    isize const usable = static_cast<isize>(mi_usable_size(p));
    isize const cap = max_bytes < mi_max_bytes ? max_bytes : mi_max_bytes;
    return usable < cap ? usable : cap;
}

isize mi_allocate_bytes(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    auto const result = mi_try_allocate_bytes(out_ptr, min_bytes, max_bytes, alignment, userdata);
    if (min_bytes > 0 && result < 0)
        cc::impl::allocation_failed(min_bytes, max_bytes, alignment);
    return result;
}

void mi_deallocate_bytes(byte* p, isize bytes, isize alignment, void* userdata)
{
    CC_UNUSED(alignment);
    CC_UNUSED(userdata);

    cc::large_allocations::impl::on_deallocate(p, bytes);
#if defined(__EMSCRIPTEN__)
    if (bytes > mi_max_bytes)
    {
        g_large_live.fetch_sub(bytes, cc::memory_order_relaxed);
        std::free(p);
        return;
    }
#endif
    // mimalloc tracks the block's size and alignment internally, so a plain mi_free suffices.
    mi_free(p);
}

isize mi_try_resize_bytes_in_place(byte* p, isize old_bytes, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    CC_UNUSED(userdata);

    CC_ASSERT(p != nullptr, "cannot resize null pointer");
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "alignment must be a power of 2");
    CC_ASSERT(old_bytes > 0, "old_bytes must be positive");
    CC_ASSERT(1 <= min_bytes && min_bytes <= max_bytes, "must have 1 <= min_bytes <= max_bytes");

    // An in-place resize succeeds iff min_bytes still fits the block's usable size, and the block never moves, so pointers into the allocation stay valid.
    // mi_usable_size rather than mi_expand: mi_expand is a documented no-op under mimalloc's padding/guard debug modes (MI_DEBUG / MI_SECURE), which would make every resize fail there.
    // mi_usable_size excludes the padding region, so reporting capacity up to it keeps writes inside the user block and leaves mimalloc's overflow canary intact.
    // A block above mi_max_bytes is emscripten's malloc's, which resizes nothing in place, and a mimalloc block never
    // grows past mi_max_bytes, which would hand it to the other allocator's free.
    if (old_bytes > mi_max_bytes)
        return -1;
    isize const block = static_cast<isize>(mi_usable_size(p));
    isize const usable = block < mi_max_bytes ? block : mi_max_bytes;
    if (min_bytes > usable)
    {
        return -1;
    }
    return usable < max_bytes ? usable : max_bytes;
}

/// mimalloc memory resource instance, data-segment resident so the default-resource
/// pointer is valid during static initialization in other translation units.
constinit cc::memory_resource const mimalloc_memory_resource = {
    .allocate_bytes = mi_allocate_bytes,
    .try_allocate_bytes = mi_try_allocate_bytes,
    .deallocate_bytes = mi_deallocate_bytes,
    .try_resize_bytes_in_place = mi_try_resize_bytes_in_place,
    .userdata = nullptr,
};

} // namespace

constinit cc::memory_resource const* const cc::default_memory_resource = &mimalloc_memory_resource;

cc::default_memory_usage cc::query_default_memory_usage()
{
    // mimalloc's own statistics, which it keeps in release builds too: its block counts are merged a page at a time,
    // its committed bytes as it commits
    mi_stats_t_decl(stats);
    if (!mi_stats_get(&stats))
        return {};
    return {.live_bytes = stats.malloc_normal.current + stats.malloc_huge.current + large_live_bytes(),
            .committed_bytes = stats.committed.current};
}

void cc::trim_thread_memory(memory_trim how)
{
    // forced, mimalloc also frees the thread's empty pages and purges every arena now, whatever the purge delay
    mi_collect(how == memory_trim::all);
}
