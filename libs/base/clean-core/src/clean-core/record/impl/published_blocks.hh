#pragma once

#include <clean-core/function/function_ref.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/impl/thread_state.hh>

// Walking what a thread has published, without stopping it.
//
// Shared by the crash dump and by the open-scope query, which want the same guarantee for the same reason: a report
// about a program that has gone wrong must not depend on the threads it is reporting about being cooperative.
//
// **A chunk's committed watermark is release-stored after the bytes it covers**, so reading up to it can never catch
// a torn event.
// The worst a live thread costs a reader is its newest event, which is a far better trade than suspending a thread
// that may hold the allocator or the loader lock.

namespace cc::rec::impl
{
/// Calls `f` for each published block of `ts`, oldest first, stopping early when it returns false.
///
/// Reads only up to each chunk's committed watermark.
/// A live chunk reports no seal pair, which event_view's interpolation already reads as "only the base pair is known".
inline void for_each_published_block(rec::impl::thread_state const& ts, cc::function_ref<bool(rec::chunk_view const&)> f)
{
    auto const info = rec::thread_info{.id = ts.tid, .index = ts.index, .name = cc::string_view(ts.name)};

    for (auto const* c = ts.queue_head.load(cc::memory_order_acquire); c != nullptr;
         c = c->next_in_thread.load(cc::memory_order_acquire))
    {
        auto const committed = c->committed.load(cc::memory_order_acquire);
        if (committed == 0)
            continue;

        // The seal pair is plain memory, published by the release store on `is_sealed`, so reading it from a live
        // chunk races the owner writing it.
        auto const is_sealed = c->is_sealed.load(cc::memory_order_acquire);

        auto const view = rec::chunk_view{
            .source = c,
            .thread = info,
            .bytes = cc::span<byte const>(c->data, isize(committed)),
            .chunk_seq = c->seq,
            .layer = c->layer,
            .base_cycles = c->base_cycles,
            .base_wall_secs = c->base_wall_secs,
            .seal_cycles = is_sealed ? c->seal_cycles : 0,
            .seal_wall_secs = is_sealed ? c->seal_wall_secs : 0,
        };

        if (!f(view))
            return;
    }
}
} // namespace cc::rec::impl
