#include <clean-core/record/async_scope.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/impl/ambient_hook.hh>
#include <clean-core/record/impl/writer_tls.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/thread/async_ambient.hh>

// Turning cc::async's ambient context into stream state.
//
// **Eager, at the restore sites, and not lazily on the next event.**
// A lazy delta would be free on the write path and wrong: a stretch of async work that records nothing would be
// attributed to whatever context preceded it, and an async scope exists precisely to show where TIME goes.
// A chain of co_awaits that logs nothing is the region you most want attributed correctly.
//
// **The delta carries the trace id, not the ambient address, and dedups on it.**
// An address is unique only while its link lives, so an earlier version pinned each head into the chunk to reserve it.
// That cost one pin per context switch against a 64-slot array, force-rotating a whole megabyte chunk every 64
// switches — a 75% tax on an async-heavy workload, bought for an identity that a value carries for free.
// An id compares soundly with no pin, no atomic and no lifetime.
//
// What a restore site pays is a short chain walk for the id, which measures as free beside the event it gates.
//
// **Leaving every context is the one lazy transition.**
// A pool worker leaves its context before each queued item and, draining one test's work, re-enters that same context
// right after, so an eager delta would be two events per item under a harness that installs owners.
// The reset is only marked; the next event writes it first, and re-entering the same context cancels it.
// What it gives up is the time between the two: a gap with no events in it stays billed to the context before it.
//
// **The owner id (cc::rec::owner_scope) is found in the same walk.**
// Until some owner_scope has ever been installed the walk stops at the trace, so a process without a harness pays one
// relaxed load for it.

namespace
{
using namespace cc::primitive_defines;

constexpr cc::rec::field ambient_fields[] = {
    {.name = "trace", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "owner", .type = cc::rec::type_code::u64_, .offset = 8, .size = 8},
};

struct ambient_payload
{
    u64 trace = 0;
    u64 owner = 0;
};

constexpr cc::rec::desc ambient_desc = {
    .kind = cc::rec::event_kind::ambient_changed,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::attribution),
    .name = "async.ambient",
    .dom = &cc::rec::g_system_domain,
    .fields = ambient_fields,
    .field_count = 2,
    .fixed_payload_size = sizeof(ambient_payload),
};
} // namespace

void cc::rec::impl::note_ambient_change(void* head)
{
    // Before the walk, so a build with attribution silenced pays one load and a branch.
    if (!rec::is_recording(ambient_desc))
        return;

    auto& w = t_writer;
    if (head == nullptr)
    {
        if (w.last_trace != 0 || w.last_owner != 0)
            w.ambient_reset_pending = true;
        return;
    }

    auto payload = ambient_payload{};
    auto const* const trace_t = rec::impl::trace_tag();
    auto const* const owner_t = rec::impl::owner_tag();
    auto found_trace = false;
    auto found_owner = !rec::impl::g_owner_ever_installed.load(cc::memory_order_relaxed);

    // The innermost link of each tag wins, and the walk ends once both are known.
    for (auto const* l = static_cast<cc::async_ambient_link const*>(head); l != nullptr; l = l->parent)
    {
        if (!found_trace && l->tag == trace_t)
        {
            payload.trace = l->value;
            found_trace = true;
        }
        else if (!found_owner && l->tag == owner_t)
        {
            payload.owner = l->value;
            found_owner = true;
        }

        if (found_trace && found_owner)
            break;
    }

    // A worker draining related items restores the same context over and over, and two different heads under one
    // attribution are the same attribution anyway — so this skips strictly more than an address compare could.
    // A pending reset is cancelled by coming back to where the thread was: no event saw the gap.
    auto const unchanged = w.last_trace == payload.trace && w.last_owner == payload.owner;
    w.ambient_reset_pending = false;
    if (unchanged)
        return;

    rec::record_event(ambient_desc, payload);
    w.last_trace = payload.trace;
    w.last_owner = payload.owner;
}

void cc::rec::impl::flush_ambient_reset()
{
    auto& w = t_writer;
    w.ambient_reset_pending = false; // first: the write below goes through the same check

    if (!rec::is_recording(ambient_desc))
        return;

    rec::record_event(ambient_desc, ambient_payload{});
    w.last_trace = 0;
    w.last_owner = 0;
}
