#pragma once

#include <clean-core/record/record.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/thread/async_ambient.hh>

// CC_RECORD_ASYNC_SCOPE — attribution that follows the work, and the trace ids that ride it.
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
// It costs more than a local scope: two refcounted link allocations, against two thread-local writes.
// That is the right trade for a request, a frame or a job, and the wrong one for an inner loop.
//
// **An async scope IS a trace, and its id is not optional.**
// Tracing wants to be infectious for exactly the reason an async scope is, so the two are one mechanism rather than
// two: propagation, naming and the state deltas all come from the chain.
// The id is what the event stream attributes BY — a scope without one would propagate a context that no recording
// could name — so every scope mints one unless handed one.
//
// The id rides as a second link beside the scope's own, which is why a scope costs two allocations.

namespace cc::rec::impl
{
/// The ambient tag an async scope installs under; the value is the scope's `desc const*`.
/// A consumer walks a chain for this tag to find which scopes an event was recorded inside.
CC_ASYNC_AMBIENT_TAG(async_scope_tag)

/// The ambient tag a trace id installs under.
///
/// The value is the id's BIT PATTERN stored in the pointer slot, not a pointer to it.
/// A trace id is 64 bits and so is the slot, so this needs no allocation and no lifetime — which matters, because the
/// link routinely outlives the scope object that pushed it.
CC_ASYNC_AMBIENT_TAG(trace_tag)

/// The layout of an async scope's begin/end payload: the trace id in effect, or 0.
inline constexpr rec::field async_scope_fields[] = {
    {.name = "trace", .type = rec::type_code::u64_, .offset = 0, .size = 8},
};

/// Pushes a trace id onto the ambient chain.
/// A `none` id installs nothing — which is not what a scope does, but is what a consumer handing one through gets.
struct trace_link_scope
{
    explicit trace_link_scope(rec::trace_id id);
    ~trace_link_scope();

    trace_link_scope(trace_link_scope const&) = delete;
    trace_link_scope& operator=(trace_link_scope const&) = delete;

private:
    // In place rather than behind a cc::unique_ptr: a `none` id must cost no allocation at all, and
    // async_ambient_scope is neither movable nor default-constructible, so there is nothing to hold it in optionally.
    bool _installed = false;
    alignas(cc::async_ambient_scope) byte _storage[sizeof(cc::async_ambient_scope)] = {};
};

/// Opens an async scope, writes its begin/end pair, and owns the links that carry it.
///
/// Order is load-bearing: the links go on before the begin event and come off after the end event, so each event sits
/// beside an ambient delta that already names the full context.
struct async_scope_guard
{
    async_scope_guard(rec::desc const& begin_desc, rec::desc const& end_desc, rec::trace_id id);
    ~async_scope_guard();

    async_scope_guard(async_scope_guard const&) = delete;
    async_scope_guard& operator=(async_scope_guard const&) = delete;

private:
    rec::desc const& _end_desc;
    rec::trace_id _id;
    rec::impl::trace_link_scope _trace;
    cc::async_ambient_scope _scope;
};
} // namespace cc::rec::impl

namespace cc::rec
{
/// The descriptor of the innermost async scope in effect on the calling thread, or null.
[[nodiscard]] inline rec::desc const* current_async_scope()
{
    return static_cast<rec::desc const*>(cc::async_ambient_lookup(impl::async_scope_tag()));
}
} // namespace cc::rec

/// Defines an async scope's two descriptors, whose shared name is what the scope is called.
///
/// Two sites rather than one site plus a kind override: a descriptor is what every consumer reads an event through,
/// and making one of them mean two things would cost each of them a special case.
#define CC_REC_DEFINE_ASYNC_SCOPE_DESCS(begin_var, end_var, name_)                                  \
    CC_REC_DEFINE_DESC(begin_var, ::cc::rec::event_kind::async_scope_begin, ::cc::rec::level::info, \
                       ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,  \
                       ::cc::rec::impl::async_scope_fields, 1, 8);                                  \
    CC_REC_DEFINE_DESC(end_var, ::cc::rec::event_kind::async_scope_end, ::cc::rec::level::info,     \
                       ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,  \
                       ::cc::rec::impl::async_scope_fields, 1, 8)

/// Opens a named async scope: everything recorded under it, on any thread, belongs to it.
///
///   CC_RECORD_ASYNC_SCOPE("load-level");
///   co_await load_meshes();                        // still attributed, on whichever worker runs it
///   auto const id = cc::rec::current_trace_id();   // to relate this operation to another
///
/// Unlike CC_RECORD_SCOPE this may cross a suspend — that is the entire point.
#define CC_RECORD_ASYNC_SCOPE(name_)                                                      \
    CC_REC_DEFINE_ASYNC_SCOPE_DESCS(cc_rec_async_desc_, cc_rec_async_end_desc_, (name_)); \
    auto const cc_rec_async_scope_                                                        \
        = ::cc::rec::impl::async_scope_guard(cc_rec_async_desc_, cc_rec_async_end_desc_, ::cc::rec::new_trace_id())

/// The same, for a scope whose id came from somewhere else — a request id off the wire, a job id from a queue.
#define CC_RECORD_ASYNC_SCOPE_WITH_ID(name_, id_)                                         \
    CC_REC_DEFINE_ASYNC_SCOPE_DESCS(cc_rec_async_desc_, cc_rec_async_end_desc_, (name_)); \
    auto const cc_rec_async_scope_                                                        \
        = ::cc::rec::impl::async_scope_guard(cc_rec_async_desc_, cc_rec_async_end_desc_, (id_))
