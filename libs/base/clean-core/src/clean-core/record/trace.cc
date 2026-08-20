#include "trace.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/thread/thread.hh>

namespace
{
using namespace cc::primitive_defines;

/// How many low bits of a trace id are the per-thread counter.
/// Forty leaves a trillion ids per thread, which no run reaches, and twenty-four bits of thread identity, which is far
/// more threads than a process starts.
constexpr int counter_bits = 40;

/// The calling thread's next counter value.
/// Thread-local, so minting takes no lock and no atomic.
thread_local u64 tl_next_counter = 1;

/// The trace the calling thread is under.
/// Not in writer_tls: that struct is on the hot path and this is read only by tracing itself.
thread_local cc::rec::trace_id tl_current_trace = cc::rec::trace_id::none;

constexpr cc::rec::desc relation_desc = {
    .kind = cc::rec::event_kind::trace_relation,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::tracing),
    .name = "trace.relation",
    .dom = &cc::rec::g_default_domain,
    .fields = cc::rec::impl::relation_fields,
    .field_count = 3,
    .fixed_payload_size = 24,
};

constexpr cc::rec::desc trace_scope_desc = {
    .kind = cc::rec::event_kind::trace_scope,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::tracing),
    .name = "trace.scope",
    .dom = &cc::rec::g_default_domain,
    .fields = cc::rec::impl::trace_scope_fields,
    .field_count = 1,
    .fixed_payload_size = 8,
};

struct relation_payload
{
    u64 from = 0;
    u64 to = 0;
    u8 kind = 0;
    u8 padding[7] = {};
};
} // namespace

cc::rec::trace_id cc::rec::new_trace_id()
{
    auto const thread = u64(cc::current_thread_id());
    auto const counter = tl_next_counter++;
    return rec::trace_id((thread << counter_bits) | (counter & ((u64(1) << counter_bits) - 1)));
}

cc::rec::trace_id cc::rec::current_trace_id()
{
    return tl_current_trace;
}

void cc::rec::record_relation(cc::rec::trace_id from, cc::rec::relation_kind kind, cc::rec::trace_id to)
{
    auto const payload = relation_payload{.from = u64(from), .to = u64(to), .kind = u8(kind)};
    rec::record_event(relation_desc, payload);
}

void cc::rec::impl::write_trace_scope(cc::rec::trace_id id)
{
    rec::record_event(trace_scope_desc, u64(id));
}

cc::rec::impl::trace_scope_guard::trace_scope_guard(cc::rec::trace_id id) : _previous(tl_current_trace)
{
    tl_current_trace = id;
    write_trace_scope(id);
}

cc::rec::impl::trace_scope_guard::~trace_scope_guard()
{
    tl_current_trace = _previous;

    // A delta, not a span end: the consumer carries the running value forward, so leaving means publishing whatever
    // the thread went back to.
    write_trace_scope(_previous);
}
