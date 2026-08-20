#include "trace.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/thread/thread.hh>

namespace
{
using namespace cc::primitive_defines;

/// How many low bits of a trace id are the per-thread counter.
/// Forty leaves a trillion ids per thread, which no run reaches, and twenty-four bits of thread identity, which is far
/// more threads than a process starts.
constexpr int counter_bits = 40;

/// How many members a relation event writes inline before it gives up.
/// Generous: a fan-in of a thousand inputs is already a design smell, and the cap keeps the payload bounded.
constexpr isize max_relation_members = 1024;

/// The calling thread's next counter value.
/// Thread-local, so minting takes no lock and no atomic.
thread_local u64 tl_next_counter = 1;

/// The trace the calling thread is under.
/// Not in writer_tls: that struct is on the hot path and this is read only by tracing itself.
thread_local cc::rec::trace_id tl_current_trace = cc::rec::trace_id::none;
} // namespace

cc::rec::desc const cc::rec::impl::trace_restore_desc = {
    .kind = cc::rec::event_kind::trace_scope,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::tracing),
    .name = "trace.restore",
    .dom = &cc::rec::g_default_domain,
    .fields = cc::rec::impl::trace_scope_fields,
    .field_count = 1,
    .fixed_payload_size = 8,
};

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

void cc::rec::impl::record_relation_members(cc::rec::desc const& d, cc::span<cc::rec::trace_id const> members)
{
    if (!rec::is_recording(d) || members.empty())
        return;

    auto const count = cc::min(members.size(), max_relation_members);
    auto const payload_bytes = isize(sizeof(u32)) + count * isize(sizeof(u64));

    auto writer = rec::open_event(d, payload_bytes);
    if (!writer.is_open())
        return;

    auto const out = writer.payload();
    if (out.size() < payload_bytes)
        return; // abandoning leaves the chunk untouched, which beats writing a half-truth

    auto const written = u32(count);
    cc::memcpy(out.data(), &written, sizeof(written));
    for (isize i = 0; i < count; ++i)
    {
        auto const id = u64(members[i]);
        cc::memcpy(out.data() + sizeof(u32) + i * isize(sizeof(u64)), &id, sizeof(id));
    }

    writer.commit(payload_bytes, count < members.size() ? rec::impl::flag_truncated : rec::impl::flag_none);
}

void cc::rec::impl::write_trace_scope(cc::rec::desc const& d, cc::rec::trace_id id)
{
    rec::record_event(d, u64(id));
}

cc::rec::impl::trace_scope_guard::trace_scope_guard(cc::rec::desc const& d, cc::rec::trace_id id)
  : _previous(tl_current_trace)
{
    tl_current_trace = id;
    write_trace_scope(d, id);
}

cc::rec::impl::trace_scope_guard::~trace_scope_guard()
{
    tl_current_trace = _previous;

    // The delta says what is in effect NOW, so leaving publishes whatever the thread went back to.
    // A separate descriptor, because the entering site's name belongs to the trace it opened.
    write_trace_scope(trace_restore_desc, _previous);
}
