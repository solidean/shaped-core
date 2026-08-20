#include "writer.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/record/chunk_pool.hh>
#include <clean-core/record/impl/system_state.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/thread/thread.hh>

using namespace cc::primitive_defines;

namespace
{
using cc::rec::impl::t_writer;

//
// The system's own events, so the cold path profiles itself
//

constexpr cc::rec::field gap_fields[] = {
    {.name = "events", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "bytes", .type = cc::rec::type_code::u64_, .offset = 8, .size = 8},
    {.name = "begin_cycles", .type = cc::rec::type_code::u64_, .offset = 16, .size = 8},
    {.name = "end_cycles", .type = cc::rec::type_code::u64_, .offset = 24, .size = 8},
};

struct gap_payload
{
    u64 events = 0;
    u64 bytes = 0;
    u64 begin_cycles = 0;
    u64 end_cycles = 0;
};

constexpr cc::rec::desc gap_desc = {
    .kind = cc::rec::event_kind::gap,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::logging),
    .name = "record.gap",
    .dom = &cc::rec::g_system_domain,
    .fields = gap_fields,
    .field_count = 4,
    .fixed_payload_size = sizeof(gap_payload),
};

constexpr cc::rec::field acquired_fields[] = {
    {.name = "cold_path_cycles", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "chunk_seq", .type = cc::rec::type_code::u64_, .offset = 8, .size = 8},
};

struct acquired_payload
{
    u64 cold_path_cycles = 0;
    u64 chunk_seq = 0;
};

constexpr cc::rec::desc acquired_desc = {
    .kind = cc::rec::event_kind::chunk_acquired,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::logging),
    .name = "record.chunk_acquired",
    .dom = &cc::rec::g_system_domain,
    .fields = acquired_fields,
    .field_count = 2,
    .fixed_payload_size = sizeof(acquired_payload),
};

/// Runs the thread exit handshake.
/// Constructed from the cold path so the hot path never sees the one-time-initialization guard it carries.
struct thread_exit_sentinel
{
    ~thread_exit_sentinel();
};
thread_local thread_exit_sentinel tl_exit_sentinel;

/// Writes one event straight at the cursor with no gating and no rotation; the space must already be there.
void write_bookkeeping(cc::rec::desc const& d, void const* payload, isize payload_size, u64 cycles)
{
    auto& w = t_writer;
    if (w.cur + cc::rec::impl::event_bytes_for(payload_size) > w.end)
        return;

    cc::rec::impl::write_event_at(d, cycles, 0, cc::rec::impl::flag_none, payload, payload_size);
}

/// Drops everything this thread remembers about a previous incarnation of the system.
/// Cheap and branch-predictable: the generation matches on every call but the first after an initialize.
void sync_generation()
{
    auto& w = t_writer;
    auto const gen = cc::rec::impl::g_system_generation.load(cc::memory_order_acquire);
    if (w.generation == gen)
        return;

    w = {};
    w.generation = gen;
}

/// Registers this thread with the consumer on its very first record.
void register_current_thread()
{
    auto& w = t_writer;
    CC_ASSERT(w.state == nullptr, "thread already registered");

    auto* const s = new cc::rec::impl::thread_state();
    s->tid = cc::current_thread_id();
    s->tls = &w;
    cc::rec::impl::register_thread_state(s);
    w.state = s;

    (void)&tl_exit_sentinel; // odr-use, so this thread runs the exit handshake
}

/// Seals `c` and publishes it, so the consumer may finish with it.
void seal_chunk(cc::rec::chunk* c, u64 cycles)
{
    c->seal_cycles = cycles;
    c->seal_wall_secs = cc::current_time_wall_secs();
    c->is_sealed.store(true, cc::memory_order_release);
}
} // namespace

cc::atomic<bool> cc::rec::impl::g_capture_core_id = true;

bool cc::rec::impl::writer_rotate(isize needed)
{
    sync_generation();

    auto& w = t_writer;
    auto* const pool = g_pool.load(cc::memory_order_acquire);
    if (pool == nullptr)
        return false;

    auto const cold_begin = cc::current_cycles();
    if (cold_begin < w.retry_at_cycles)
        return false;

    if (w.state == nullptr)
        register_current_thread();

    // Seal the outgoing chunk first, so the consumer can finish with it while we are still waiting on the pool.
    // Taken from the queue tail rather than from the cursor: seal_current_thread_chunk may already have sealed it, and
    // the queue must stay one unbroken chain whatever the cursor is doing.
    auto* const outgoing = w.state->produce_tail;
    if (outgoing != nullptr && !outgoing->is_sealed.load(cc::memory_order_relaxed))
        seal_chunk(outgoing, cold_begin);

    w.cur = nullptr;
    w.end = nullptr;
    w.current = nullptr;

    auto const layer = w.layer_plus_one == 0 ? rec::chunk::no_layer : u16(w.layer_plus_one - 1);
    auto* const fresh = pool->acquire(w.state, w.state->next_seq, layer);
    if (fresh == nullptr)
    {
        w.retry_at_cycles = cold_begin + g_drop_retry_cycles.load(cc::memory_order_relaxed);
        return false;
    }
    ++w.state->next_seq;

    // The producer reference travels with the chunk into the queue; the consumer drops it once it is done.
    if (outgoing != nullptr)
        outgoing->next_in_thread.store(fresh, cc::memory_order_release);
    else
        w.state->queue_head.store(fresh, cc::memory_order_release);
    w.state->produce_tail = fresh;

    w.current = fresh;
    w.cur = fresh->data;
    w.end = fresh->data + fresh->capacity;
    w.retry_at_cycles = 0;

    // A fresh chunk starts from no known state, so the next ambient write is unconditional.
    w.last_ambient = nullptr;

    if (w.state->dropped_events > 0)
    {
        auto const gap = gap_payload{
            .events = w.state->dropped_events,
            .bytes = w.state->dropped_bytes,
            .begin_cycles = w.state->drop_begin_cycles,
            .end_cycles = w.state->drop_end_cycles,
        };
        write_bookkeeping(gap_desc, &gap, isize(sizeof(gap)), cold_begin);

        w.state->dropped_events = 0;
        w.state->dropped_bytes = 0;
        w.state->drop_begin_cycles = 0;
        w.state->drop_end_cycles = 0;
    }

    // Bracketed by two readings, so the cold path reports its own cost rather than being modelled.
    auto const cold_end = cc::current_cycles();
    auto const acquired = acquired_payload{.cold_path_cycles = cold_end - cold_begin, .chunk_seq = fresh->seq};
    write_bookkeeping(acquired_desc, &acquired, isize(sizeof(acquired)), cold_end);

    return w.cur + needed <= w.end;
}

void cc::rec::impl::writer_account_drop(isize bytes, u64 cycles)
{
    auto& w = t_writer;
    if (w.state == nullptr)
        return;

    if (w.state->dropped_events == 0)
        w.state->drop_begin_cycles = cycles;
    w.state->drop_end_cycles = cycles;
    ++w.state->dropped_events;
    w.state->dropped_bytes += u64(bytes);
}

cc::rec::event_writer cc::rec::open_event(cc::rec::desc const& d, isize max_payload)
{
    rec::event_writer e;
    if (!rec::is_recording(d))
        return e;

    auto& w = impl::t_writer;
    auto const header_bytes = isize(sizeof(impl::event_header));

    // A rotation is worth it only when the current chunk cannot hold a useful payload at all.
    // Otherwise a long message is better truncated than allowed to abandon most of a megabyte.
    if (w.cur + header_bytes + impl::padded_payload(1) > w.end)
    {
        if (!impl::writer_rotate(header_bytes + impl::padded_payload(1)))
        {
            impl::writer_account_drop(header_bytes, cc::current_cycles());
            return e;
        }
    }

    // After the rotation, so this event never sorts before the bookkeeping the cold path just wrote.
    u16 core = 0;
    auto const cycles = impl::record_timestamp(core);

    auto const available = (w.end - w.cur) - header_bytes;
    e._base = w.cur;
    e._capacity = cc::min(max_payload, available);
    e._desc = &d;
    e._cycles = cycles;
    e._core = core;
    return e;
}

void cc::rec::event_writer::commit(isize payload_size, u16 extra_flags)
{
    if (_base == nullptr)
        return;

    auto flags = extra_flags;
    if (payload_size > _capacity)
    {
        payload_size = _capacity;
        flags |= rec::impl::flag_truncated;
    }

    auto& w = rec::impl::t_writer;
    CC_ASSERT(w.cur == _base, "another event was recorded while this writer was open");

    // The payload is already in place, so this stamps the header and publishes in one step.
    // Deliberately not via write_event_at: that would publish a zero-length event first and then extend it, which a
    // consumer reading the live chunk could catch halfway.
    auto* const header = reinterpret_cast<rec::impl::event_header*>(_base);
    header->desc = _desc;
    header->cycles = _cycles;
    header->payload_size = u32(payload_size);
    header->core = _core;
    header->flags = flags;

    w.cur = _base + rec::impl::event_bytes_for(payload_size);
    w.current->committed.store(u32(w.cur - w.current->data), cc::memory_order_release);

    _base = nullptr;
}

cc::rec::impl::listener_layer_scope::listener_layer_scope(u16 layer)
{
    auto& w = t_writer;
    _saved_cur = w.cur;
    _saved_end = w.end;
    _saved_current = w.current;
    _saved_state = w.state;
    _saved_layer_plus_one = w.layer_plus_one;

    if (w.layer_depth == 0)
    {
        // A chunk carries one layer, so a layer change retires the previous listener chunk rather than mixing into it.
        if (w.alt_current != nullptr && w.alt_current->layer != layer)
        {
            seal_chunk(w.alt_current, cc::current_cycles());
            w.alt_current = nullptr;
            w.alt_cur = nullptr;
            w.alt_end = nullptr;
        }

        w.cur = w.alt_cur;
        w.end = w.alt_end;
        w.current = w.alt_current;
        w.state = w.alt_state;
        w.layer_plus_one = u16(layer + 1);
    }
    else
    {
        // Nested dispatch: record nothing, so the layer rule cannot cycle.
        w.cur = nullptr;
        w.end = nullptr;
        w.current = nullptr;
    }

    ++w.layer_depth;
}

cc::rec::impl::listener_layer_scope::~listener_layer_scope()
{
    auto& w = t_writer;
    --w.layer_depth;

    if (w.layer_depth == 0)
    {
        w.alt_cur = w.cur;
        w.alt_end = w.end;
        w.alt_current = w.current;
        w.alt_state = w.state;
    }

    w.cur = _saved_cur;
    w.end = _saved_end;
    w.current = _saved_current;
    w.state = _saved_state;
    w.layer_plus_one = _saved_layer_plus_one;
}

void cc::rec::seal_current_thread_chunk()
{
    // Before touching anything: a thread that last recorded under a previous incarnation holds pointers into a pool
    // that has since been freed, and this runs from a thread-exit handshake with no cold path ahead of it.
    sync_generation();

    auto& w = impl::t_writer;
    auto const now = cc::current_cycles();

    // Only the cursors are cleared; `current` and `produce_tail` stay, so the next rotation still links the chain.
    if (w.current != nullptr && !w.current->is_sealed.load(cc::memory_order_relaxed))
        seal_chunk(w.current, now);
    if (w.alt_current != nullptr && !w.alt_current->is_sealed.load(cc::memory_order_relaxed))
        seal_chunk(w.alt_current, now);

    w.cur = nullptr;
    w.end = nullptr;
    w.alt_cur = nullptr;
    w.alt_end = nullptr;
}

namespace
{
thread_exit_sentinel::~thread_exit_sentinel()
{
    cc::rec::seal_current_thread_chunk();

    auto& w = t_writer;
    if (w.state != nullptr)
        w.state->is_alive.store(false, cc::memory_order_release);
    if (w.alt_state != nullptr)
        w.alt_state->is_alive.store(false, cc::memory_order_release);
}
} // namespace

void cc::rec::set_current_thread_record_name(cc::string_view name)
{
    sync_generation();

    auto& w = impl::t_writer;
    if (w.state == nullptr)
        register_current_thread();

    auto const n = cc::min(name.size(), isize(sizeof(w.state->name)) - 1);
    for (isize i = 0; i < n; ++i)
        w.state->name[i] = name[i];
    w.state->name[n] = '\0';
}
