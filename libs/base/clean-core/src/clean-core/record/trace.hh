#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/record/record.hh>

// Correlating work that neither the thread stack nor the clock relates.
//
// A profiling scope answers "what was this thread doing", and it nests because a call stack does.
// Tracing answers the harder question: **which of these events belong to the same logical operation**.
// That operation spans threads, queues, retries and caches, and its parts have no lexical relationship at all.
//
// The whole mechanism is two things:
//
//   * an ID that costs nothing to mint — a per-thread counter, no allocation, no registry, no lock;
//   * a RELATION event saying that some ids are related, and how.
//
// **The graph is reconstructed entirely offline.** Nothing here builds one.
// That is what makes a late discovery free: when a computation turns out to have produced a cache key another
// operation already used, you record `same_key_as` the moment you learn it, and the reconstruction does not care that
// it arrived last.
// An id that nothing tracks also cannot leak, cannot be looked up wrongly, and costs nothing to abandon.
//
// -------------------------------------------------------------------------------------------------------
// CC_TRACE_SCOPE is INTERIM and will be replaced
// -------------------------------------------------------------------------------------------------------
//
// A trace wants to be infectious: a request or a job spans threads, and every piece of work spawned under it belongs
// to it wherever it ends up running.
// That is exactly what cc::async's ambient chain already does, so a trace scope should BE an ambient scope carrying
// an id, and then propagation, naming and the state deltas are one mechanism rather than two.
//
// That chain now carries async scopes (clean-core/record/async_scope.hh), so what remains is giving one an optional
// id and deleting the thread-local path below.
// Until then this scope is correct for synchronous work and silently under-attributes the moment the work suspends:
// a `co_await` moves the continuation to another thread, and this scope does not follow it.
// Ids and relations are unaffected either way — those are complete, and only attribution moves.

/// The relation types that come up everywhere.
/// Define your own next to the code that records it; these are only the ones with no better home.
namespace cc::rec
{
/// The subject spawned the objects.
inline constexpr rec::relation_type relation_parent_of = {
    .name = "parent_of",
    .inverse_name = "child_of",
    .is_transitive = true,
};

/// The objects happened because of the subject, without the subject having created them.
inline constexpr rec::relation_type relation_caused_by = {
    .name = "caused_by",
    .inverse_name = "caused",
};

/// The members turned out to be the same work — a shared cache key, a deduplicated request.
/// An equivalence, so a reader may merge them into one logical operation.
inline constexpr rec::relation_type relation_same_key_as = {
    .name = "same_key_as",
    .is_symmetric = true,
    .is_transitive = true,
    .is_equivalence = true,
};

/// The objects are later attempts at the subject's work, as in a retry.
inline constexpr rec::relation_type relation_follows = {
    .name = "follows",
    .inverse_name = "preceded_by",
    .is_transitive = true,
};

/// Mints a fresh id.
///
/// The thread's identity in the high bits and a per-thread counter in the low ones, so two threads can never collide
/// and neither takes a lock.
[[nodiscard]] rec::trace_id new_trace_id();

/// The trace the calling thread is currently under, or `none`.
[[nodiscard]] rec::trace_id current_trace_id();
} // namespace cc::rec

namespace cc::rec::impl
{
/// The layout of a relation event: a count, then that many ids.
///
/// N-ary rather than binary, because several relations are naturally so — five requests that hit one cache key, eight
/// inputs to one join — and decomposing those into pairs against a representative loses the fact that they were
/// related AS A GROUP.
/// Binary is just the two-member case, and costs the same bytes it did as a fixed pair.
inline constexpr rec::field relation_fields[] = {
    {.name = "members", .type = rec::type_code::u64_array, .offset = 0, .size = 4},
};

/// The layout of a trace-scope event, which publishes the id now in effect.
inline constexpr rec::field trace_scope_fields[] = {
    {.name = "trace", .type = rec::type_code::u64_, .offset = 0, .size = 8},
};

/// Writes one relation event.
/// **The first member is the SUBJECT and the rest are objects**, which is what lets one convention cover a fan-out
/// (`parent_of(parent, children…)`) and a fan-in (`caused_by(effect, causes…)`) alike.
/// Order is meaningless for a symmetric type, where every member is a peer.
void record_relation_members(rec::desc const& d, cc::span<rec::trace_id const> members);

/// Publishes "this thread is now under `id`".
void write_trace_scope(rec::desc const& d, rec::trace_id id);

/// The descriptor a scope uses on the way out, when it publishes whatever the thread went back to.
/// A separate site because the entering site's name belongs to the trace it entered, not to the one it restored.
extern rec::desc const trace_restore_desc;

/// Puts the calling thread under a trace for a scope, restoring the previous one on the way out.
struct trace_scope_guard
{
    trace_scope_guard(rec::desc const& d, rec::trace_id id);
    ~trace_scope_guard();

    trace_scope_guard(trace_scope_guard const&) = delete;
    trace_scope_guard& operator=(trace_scope_guard const&) = delete;

private:
    rec::trace_id _previous;
};
} // namespace cc::rec::impl

/// Defines a relation site's descriptor.
/// The relation type lives in the DESCRIPTOR rather than the payload, so the event costs no bytes for it and the
/// serializer resolves it exactly as it resolves a unit.
#define CC_REC_DEFINE_RELATION_DESC(var_name, type_)                           \
    static constexpr ::cc::rec::desc var_name = {                              \
        .kind = ::cc::rec::event_kind::trace_relation,                         \
        .enable_bit = ::cc::rec::enable_bit_of(::cc::rec::category::tracing),  \
        .name = (type_).name,                                                  \
        .relation = &(type_),                                                  \
        .dom = cc_rec_domain(),                                                \
        .site = ::cc::rec::source_ref::from(::cc::source_location::current()), \
        .fields = ::cc::rec::impl::relation_fields,                            \
        .field_count = 1,                                                      \
        .fixed_payload_size = ::cc::rec::desc::variable_payload,               \
    }

/// Records that some trace ids are related, and how.
///
///   CC_RECORD_RELATION(cc::rec::relation_parent_of, request, fetch);
///   CC_RECORD_RELATION(cc::rec::relation_same_key_as, a, b, c, d);
///
/// The first id is the subject; the rest are objects.
#define CC_RECORD_RELATION(type_, ...)                                                \
    do                                                                                \
    {                                                                                 \
        CC_REC_DEFINE_RELATION_DESC(cc_rec_site_desc_, (type_));                      \
        ::cc::rec::trace_id const cc_rec_members_[] = {__VA_ARGS__};                  \
        ::cc::rec::impl::record_relation_members(cc_rec_site_desc_, cc_rec_members_); \
    } while (false)

/// The same, for a member list only known at runtime.
#define CC_RECORD_RELATION_MANY(type_, members_)                                 \
    do                                                                           \
    {                                                                            \
        CC_REC_DEFINE_RELATION_DESC(cc_rec_site_desc_, (type_));                 \
        ::cc::rec::impl::record_relation_members(cc_rec_site_desc_, (members_)); \
    } while (false)

/// Defines a trace scope's descriptor, whose name is what the trace is called.
#define CC_REC_DEFINE_TRACE_DESC(var_name, name_)                              \
    static constexpr ::cc::rec::desc var_name = {                              \
        .kind = ::cc::rec::event_kind::trace_scope,                            \
        .enable_bit = ::cc::rec::enable_bit_of(::cc::rec::category::tracing),  \
        .name = (name_),                                                       \
        .dom = cc_rec_domain(),                                                \
        .site = ::cc::rec::source_ref::from(::cc::source_location::current()), \
        .fields = ::cc::rec::impl::trace_scope_fields,                         \
        .field_count = 1,                                                      \
        .fixed_payload_size = 8,                                               \
    }

/// Opens a named trace on this thread, minting its id.
///
///   CC_TRACE_SCOPE("handle-request");
///   auto const id = cc::rec::current_trace_id();   // to relate it to something else
///
/// Naming it is the point: a bare id is an opaque number, and a viewer showing "0x0800000000000003" helps nobody.
/// Interim, and thread-local only — see the header comment.
#define CC_TRACE_SCOPE(name_)                            \
    CC_REC_DEFINE_TRACE_DESC(cc_rec_trace_desc_, name_); \
    auto const cc_rec_trace_scope_ = ::cc::rec::impl::trace_scope_guard(cc_rec_trace_desc_, ::cc::rec::new_trace_id())

/// The same, for a trace whose id came from somewhere else — a request id off the wire, a job id from a queue.
#define CC_TRACE_SCOPE_WITH_ID(name_, id_)               \
    CC_REC_DEFINE_TRACE_DESC(cc_rec_trace_desc_, name_); \
    auto const cc_rec_trace_scope_ = ::cc::rec::impl::trace_scope_guard(cc_rec_trace_desc_, (id_))
