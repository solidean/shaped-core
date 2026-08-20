#include <clean-core/record/chunk.hh>
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
// The cost this does impose is one compare at each restore site, against a value already loaded to restore from.
// A node with no token never reaches it; see clean-core/thread/async_ambient.hh.

namespace
{
using namespace cc::primitive_defines;

constexpr cc::rec::field ambient_fields[] = {
    {.name = "ambient", .type = cc::rec::type_code::pointer, .offset = 0, .size = 8},
};

constexpr cc::rec::desc ambient_desc = {
    .kind = cc::rec::event_kind::ambient_changed,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::profiling),
    .name = "async.ambient",
    .dom = &cc::rec::g_system_domain,
    .fields = ambient_fields,
    .field_count = 1,
    .fixed_payload_size = 8,
};

/// What a chunk does with a pinned ambient link when it is recycled.
void release_ambient(void* link)
{
    cc::impl::async_ambient_unobserve(link);
}
} // namespace

void cc::rec::impl::note_ambient_change(void* head)
{
    auto& w = t_writer;

    // A worker draining related items restores the same context over and over, and the header at the call site says so.
    // That repeat costs a compare and nothing else.
    if (w.last_ambient == head)
        return;

    if (!rec::is_recording(ambient_desc))
        return;

    auto const bytes = event_bytes_for(isize(sizeof(void*)));
    if (!writer_reserve_event_and_pin(bytes))
    {
        writer_account_drop(bytes, cc::current_cycles());
        return;
    }

    // The event names a link the recording may outlive, so the chunk holds a reference until it is recycled.
    // Retaining the head retains the WHOLE CHAIN in O(1), because a link's parent reference is strong — which is what
    // makes this affordable at all.
    // As an OBSERVER, so nothing reads the recording's grip on a context as work still in flight.
    if (head != nullptr)
    {
        cc::impl::async_ambient_observe(head);
        if (!w.current->try_add_pin({.object = head, .release = &release_ambient}))
        {
            // Reserved above, so this cannot happen; releasing rather than leaking is what to do if it ever does.
            cc::impl::async_ambient_unobserve(head);
            return;
        }
    }

    u16 core = 0;
    auto const cycles = record_timestamp(core);
    write_event_at(ambient_desc, cycles, core, flag_none, &head, isize(sizeof(head)));

    w.last_ambient = head;
}
