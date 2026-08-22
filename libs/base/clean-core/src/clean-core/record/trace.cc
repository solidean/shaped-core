#include "trace.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/thread/thread.hh>

namespace
{
using namespace cc::primitive_defines;

/// How many low bits of a trace id are the per-thread counter.
///
/// Forty bits of counter — a trillion ids per thread, which no run reaches — and twenty-four of thread identity, far
/// more threads than a process starts.
///
/// One split on every target, because the ambient slot a minted id rides in is sixty-four bits everywhere; see
/// cc::async_ambient_link.
/// Following the POINTER width instead would be silently lossy rather than loud, and would not help anyway: an id that
/// came off the wire is a full 64-bit value that no minting rule constrains.
constexpr int counter_bits = 40;
constexpr int thread_bits = 24;

static_assert(counter_bits + thread_bits <= int(sizeof(u64)) * 8,
              "a trace id must fit in cc::async's ambient slot, which is 64 bits");

/// How many members a relation event writes inline before it gives up.
/// Generous: a fan-in of a thousand inputs is already a design smell, and the cap keeps the payload bounded.
constexpr isize max_relation_members = 1024;

/// The calling thread's next counter value.
/// Thread-local, so minting takes no lock and no atomic.
thread_local u64 tl_next_counter = 1;
} // namespace

cc::rec::trace_id cc::rec::new_trace_id()
{
    auto const thread = u64(cc::current_thread_id()) & ((u64(1) << thread_bits) - 1);
    auto const counter = tl_next_counter++;
    return rec::trace_id((thread << counter_bits) | (counter & ((u64(1) << counter_bits) - 1)));
}

cc::rec::trace_id cc::rec::current_trace_id()
{
    // The chain, not a thread-local: a trace has to survive a co_await, and a thread-local cannot.
    return rec::trace_id(cc::async_ambient_lookup(rec::impl::trace_tag()));
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
