#pragma once

#include <clean-core/record/record.hh>

// Correlating work that neither the thread stack nor the clock relates.
//
// A profiling scope answers "what was this thread doing", and it nests because a call stack does.
// Tracing answers the harder question: **which of these events belong to the same logical operation**, when that
// operation spans threads, queues, retries and caches, and its parts have no lexical relationship at all.
//
// The whole mechanism is two things:
//
//   * an ID that costs nothing to mint — a per-thread counter, no allocation, no registry, no lock;
//   * a RELATION event saying two ids are related.
//
// **The graph is reconstructed entirely offline.** Nothing here builds one, and that is what makes a late discovery
// free: when a computation turns out to have produced a cache key another operation already used, you record the
// relation at the moment you learn it, and the reconstruction does not care that it arrived last.
//
// What is NOT here yet: putting an ASYNC scope under a trace, so work follows the id to whichever worker picks it up.
// That rides cc::async's ambient chain and lands with the ambient deltas.
// CC_TRACE_SCOPE below is the thread-local half, which is exact for synchronous work and simply does not follow a
// co_await.

namespace cc::rec
{
/// Mints a fresh id.
///
/// The thread's identity in the high bits and a per-thread counter in the low ones, so two threads can never collide
/// and neither takes a lock.
[[nodiscard]] rec::trace_id new_trace_id();

/// The trace the calling thread is currently under, or `none`.
[[nodiscard]] rec::trace_id current_trace_id();

/// Records that `from` and `to` are related.
///
/// Order matters for the asymmetric kinds; `same_key_as` reads either way.
/// Recording the same relation twice is harmless — the reconstruction is a set, not a list.
void record_relation(rec::trace_id from, rec::relation_kind kind, rec::trace_id to);
} // namespace cc::rec

namespace cc::rec::impl
{
/// The layout of a relation event.
inline constexpr rec::field relation_fields[] = {
    {.name = "from", .type = rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "to", .type = rec::type_code::u64_, .offset = 8, .size = 8},
    {.name = "kind", .type = rec::type_code::u8_, .offset = 16, .size = 1},
};

/// The layout of a trace-scope event, which is a state DELTA rather than a span.
inline constexpr rec::field trace_scope_fields[] = {
    {.name = "trace", .type = rec::type_code::u64_, .offset = 0, .size = 8},
};

/// Publishes "this thread is now under `id`", which is what a consumer carries forward.
void write_trace_scope(rec::trace_id id);

/// Puts the calling thread under a trace for a scope, restoring the previous one on the way out.
///
/// Thread-local and strictly LIFO, like a profiling scope, and with the same limit: it does not follow a `co_await`.
struct trace_scope_guard
{
    explicit trace_scope_guard(rec::trace_id id);
    ~trace_scope_guard();

    trace_scope_guard(trace_scope_guard const&) = delete;
    trace_scope_guard& operator=(trace_scope_guard const&) = delete;

private:
    rec::trace_id _previous;
};
} // namespace cc::rec::impl

/// Puts the calling thread under `id_` for the enclosing block, so everything it records is attributed to that trace.
///
///   auto const id = cc::rec::new_trace_id();
///   CC_TRACE_SCOPE(id);
///
/// Synchronous only — see the header comment.
#define CC_TRACE_SCOPE(id_) auto const cc_rec_trace_scope_ = ::cc::rec::impl::trace_scope_guard(id_)
