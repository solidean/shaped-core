#pragma once

#include <clean-core/record/record.hh>
#include <clean-core/thread/async_ambient.hh>

// CC_RECORD_ASYNC_SCOPE — attribution that follows the work.
//
// A CC_RECORD_SCOPE is a stack frame: one thread, one nesting depth, and a hard rule that it must not cross a
// `co_await`. That is right for a span of straight-line work and useless for a logical task.
//
// An async scope is the other half.
// It rides cc::async's ambient chain, so every node spawned under it carries it to whichever worker picks it up, across
// suspends and across threads.
// It nests logically rather than as a stack frame, and popping it does NOT require the work started under it to have
// finished — which is what makes prewarming and fire-and-forget legal.
//
// It costs more than a local scope: one refcounted link allocation, against two thread-local writes.
// That is the right trade for a request, a frame or a job, and the wrong one for an inner loop.

namespace cc::rec::impl
{
/// The ambient tag an async scope installs under.
/// A consumer walks a chain for this tag to find which scopes an event was recorded inside.
CC_ASYNC_AMBIENT_TAG(async_scope_tag)
} // namespace cc::rec::impl

namespace cc::rec
{
/// The descriptor of the innermost async scope in effect on the calling thread, or null.
[[nodiscard]] inline rec::desc const* current_async_scope()
{
    return static_cast<rec::desc const*>(cc::async_ambient_lookup(impl::async_scope_tag()));
}
} // namespace cc::rec

/// Opens a named async scope: everything recorded under it, on any thread, belongs to it.
///
///   CC_RECORD_ASYNC_SCOPE("load-level");
///   co_await load_meshes();     // still attributed, on whichever worker runs it
///
/// Unlike CC_RECORD_SCOPE this may cross a suspend — that is the entire point.
#define CC_RECORD_ASYNC_SCOPE(name_)                                                                               \
    CC_REC_DEFINE_DESC(cc_rec_async_desc_, ::cc::rec::event_kind::scope_begin, ::cc::rec::level::info,             \
                       ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr, nullptr, 0, 0); \
    auto const cc_rec_async_scope_ = ::cc::async_ambient_scope(::cc::rec::impl::async_scope_tag(),                 \
                                                               const_cast<::cc::rec::desc*>(&cc_rec_async_desc_))
