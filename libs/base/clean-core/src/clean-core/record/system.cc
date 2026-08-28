#include "system.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/chunk_pool.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/impl/system_state.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/record/impl/writer_tls.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

using namespace cc::primitive_defines;


namespace
{
using cc::rec::impl::thread_state;

//
// The thread registry
//

struct registry
{
    thread_state* head = nullptr;
    isize count = 0;
};
cc::mutex<registry> g_registry;

//
// Listeners and the one processing mutex
//

struct registered_listener
{
    cc::rec::listener* l = nullptr;
    u64 generation = 0;
};

struct processing
{
    cc::vector<registered_listener> listeners;
    u64 next_generation = 1;
    u64 chunks_processed = 0;
    u64 events_processed = 0;
};

/// **The** processing mutex.
/// Every listener callback runs under it, so a listener never needs a lock and never sees two callbacks at once.
/// It also guards registration, which is what makes unregister_listener's "no callback can still be running" true.
cc::mutex<processing> g_processing;

cc::rec::config g_config;
cc::atomic<bool> g_initialized = false;
cc::atomic<double> g_cycles_per_second = 0;

//
// Calibration
//

/// Measures the cycle counter against the steady clock.
/// Short on purpose: it runs inside initialize(), and the sealed-chunk path never needs the result.
/// The cycle rate, measured at most once per process.
///
/// Calibrating busy-waits for two milliseconds, and a process may initialize the recorder many times — a test binary
/// hands the singleton to every test that drives it itself.
/// The rate is a property of the machine rather than of an incarnation, so re-measuring it buys nothing and costs two
/// milliseconds of a core every time.
double measure_cycle_rate()
{
    static double const cached = []
    {
        // No has_cycle_counter() gate: that asks whether the tick is CHEAP, and cc::current_cycles() is monotonic on
        // every platform now — including the ones where it is a call.
        // Gating on it here is what left every WASM event stamped 0 and every duration meaningless.
        auto const t0 = cc::current_time_steady_secs();
        auto const c0 = cc::current_cycles();

        // A busy wait rather than a sleep: a couple of milliseconds of accuracy is worth more here than the core is.
        while (cc::current_time_steady_secs() - t0 < 0.002)
        {
        }

        auto const c1 = cc::current_cycles();
        auto const t1 = cc::current_time_steady_secs();

        auto const dt = t1 - t0;
        return dt > 0 ? double(c1 - c0) / dt : 0.0;
    }();

    return cached;
}

//
// Draining
//

cc::rec::thread_info info_of(thread_state const& ts)
{
    return {.id = ts.tid, .index = ts.index, .name = cc::string_view(ts.name)};
}

/// Offers one block of one thread's events to every listener whose layer may see it.
void dispatch(processing& p, cc::rec::chunk const& c, thread_state const& ts, u32 from, u32 to)
{
    auto const view = cc::rec::chunk_view{
        .source = &c,
        .thread = info_of(ts),
        .bytes = cc::span<byte const>(c.data + from, isize(to - from)),
        .chunk_seq = c.seq,
        .layer = c.layer,
        .base_cycles = c.base_cycles,
        .base_wall_secs = c.base_wall_secs,
        .seal_cycles = c.seal_cycles,
        .seal_wall_secs = c.seal_wall_secs,
    };

    for (isize i = 0; i < p.listeners.size(); ++i)
    {
        auto const& r = p.listeners[i];
        if (r.l == nullptr)
            continue;

        // The layer rule: events recorded from inside listener `k` are offered only to listeners below `k`.
        if (c.layer != cc::rec::chunk::no_layer && i >= isize(c.layer))
            continue;

        auto const layer_scope = cc::rec::impl::listener_layer_scope(u16(i));
        r.l->on_chunk(view);
    }

    ++p.chunks_processed;
    for (auto it = view.begin(); it != view.end(); ++it)
        ++p.events_processed;
}

/// Walks one thread's chunk queue as far as it has published, dispatching what is new.
/// Drains everything `ts` has published, and reports whether nothing will ever follow it.
/// The second half is what lets a dead thread's state be reaped; a live thread always answers false.
bool drain_thread(processing& p, thread_state& ts)
{
    auto const is_dead = !ts.is_alive.load(cc::memory_order_acquire);

    if (ts.consume_cursor == nullptr)
    {
        ts.consume_cursor = ts.queue_head.load(cc::memory_order_acquire);
        ts.consume_offset = 0;
        if (ts.consume_cursor == nullptr)
            return is_dead; // a thread that died without ever recording
    }

    for (;;)
    {
        auto* const c = ts.consume_cursor;

        // Nothing to stamp: the chunk's own first event is its preamble, written by the producer at rotation.
        auto const committed = c->committed.load(cc::memory_order_acquire);
        if (committed > ts.consume_offset)
        {
            dispatch(p, *c, ts, ts.consume_offset, committed);
            ts.consume_offset = committed;
        }

        if (!c->is_sealed.load(cc::memory_order_acquire))
            return false;

        auto* const next = c->next_in_thread.load(cc::memory_order_acquire);
        if (next == nullptr)
        {
            // Sealed and last in the chain.
            // For a live thread that is "between chunks"; for a dead one it is the end of everything it will say.
            return is_dead;
        }

        ts.consume_cursor = next;
        ts.consume_offset = 0;
        c->release_ref(); // the producer's reference travelled with the chunk; this is where it ends
    }
}

/// One full pass over every registered thread.
/// Called with `g_processing` held, and never with the registry lock held — a listener that records would register a
/// thread from inside the callback and deadlock on it.
/// Reused across polls rather than allocated per poll: the actor runs a thousand times a second, and it has nothing
/// else to do most of the time.
cc::vector<thread_state*> g_drain_snapshot;

void drain_all(processing& p)
{
    auto& snapshot = g_drain_snapshot;
    snapshot.clear();
    g_registry.lock(
        [&](registry& r)
        {
            snapshot.reserve(r.count);
            for (auto* s = r.head; s != nullptr; s = s->registry_next)
                snapshot.push_back(s);
        });

    // Reaped here rather than left until shutdown.
    // A thread that dies keeps its state so its last chunks stay drainable, and holding it forever would make the
    // registry grow with every thread a process ever started — which the actor then walks, a thousand times a second.
    for (auto* s : snapshot)
        if (drain_thread(p, *s))
        {
            for (auto* c = s->consume_cursor; c != nullptr;)
            {
                auto* const next = c->next_in_thread.load(cc::memory_order_acquire);
                c->release_ref();
                c = next;
            }
            s->consume_cursor = nullptr;
            s->queue_head.store(nullptr, cc::memory_order_relaxed);
            s->produce_tail = nullptr;
            cc::rec::impl::reclaim_thread_state(s);
        }

    for (isize i = 0; i < p.listeners.size(); ++i)
    {
        if (p.listeners[i].l == nullptr)
            continue;
        auto const layer_scope = cc::rec::impl::listener_layer_scope(u16(i));
        p.listeners[i].l->on_batch_end();
    }
}

//
// The background worker
//

cc::atomic<bool> g_worker_stop = false;
cc::atomic<bool> g_worker_running = false;

/// The background consumer.
///
/// A poll rather than a condition variable, because there is nothing to wait ON: producers publish by storing a
/// watermark and must never pay for a notify.
/// One millisecond of latency is the price, and shutdown pays it once.
void worker_body(double poll_interval_secs)
{
    cc::set_current_thread_name("cc-record");
    cc::rec::set_current_thread_record_name("cc-record");

    while (!g_worker_stop.load(cc::memory_order_acquire))
    {
        cc::this_thread_sleep_secs(poll_interval_secs);

        auto* const pool = cc::rec::impl::g_pool.load(cc::memory_order_acquire);
        if (pool != nullptr)
            pool->refill(g_config.ready_chunks);

        g_processing.lock([](processing& p) { drain_all(p); });
    }

    g_worker_running.store(false, cc::memory_order_release);
}

#if CC_HAS_THREADS
std::thread g_worker;

void start_worker_thread(double poll_interval_secs)
{
    g_worker = std::thread(worker_body, poll_interval_secs);
}

void join_worker_thread()
{
    if (g_worker.joinable())
        g_worker.join();
}
#else
void start_worker_thread(double)
{
    // No thread to start; draining happens on whichever thread flushes.
}

void join_worker_thread()
{
}
#endif
} // namespace

cc::atomic<cc::rec::chunk_pool*> cc::rec::impl::g_pool = nullptr;
cc::atomic<u64> cc::rec::impl::g_drop_retry_cycles = 0;
cc::atomic<u64> cc::rec::impl::g_system_generation = 1;

void cc::rec::impl::register_thread_state(thread_state* s)
{
    g_registry.lock(
        [&](registry& r)
        {
            s->index = u32(r.count);
            s->registry_next = r.head;
            r.head = s;
            ++r.count;
        });
}

void cc::rec::impl::for_each_thread_state(cc::function_ref<void(thread_state&)> f)
{
    g_registry.lock(
        [&](registry& r)
        {
            for (auto* s = r.head; s != nullptr; s = s->registry_next)
                f(*s);
        });
}

bool cc::rec::impl::try_for_each_thread_state(cc::function_ref<void(thread_state&)> f)
{
    return g_registry.try_lock(
        [&](registry& r)
        {
            for (auto* s = r.head; s != nullptr; s = s->registry_next)
                f(*s);
        });
}

isize cc::rec::impl::thread_state_count()
{
    return g_registry.lock([](registry const& r) { return r.count; });
}

isize cc::rec::impl::with_nth_thread_state(isize n, cc::function_ref<void(thread_state&)> f)
{
    return g_registry.lock(
        [&](registry& r)
        {
            if (r.count <= 0)
                return isize(0);

            auto const total = isize(r.count);
            auto const wanted = ((n % total) + total) % total;

            auto position = isize(0);
            for (auto* s = r.head; s != nullptr; s = s->registry_next, ++position)
                if (position == wanted)
                {
                    f(*s);
                    break;
                }

            return total;
        });
}

void cc::rec::impl::detach_thread_state_tls(thread_state* s)
{
    // Under the registry lock, because shutdown reads `tls` while holding it and would otherwise race an exiting
    // thread to a pointer that is about to name freed storage.
    g_registry.lock([&](registry&) { s->tls = nullptr; });
}

void cc::rec::impl::reclaim_thread_state(thread_state* s)
{
    g_registry.lock(
        [&](registry& r)
        {
            thread_state** link = &r.head;
            while (*link != nullptr && *link != s)
                link = &(*link)->registry_next;
            if (*link == s)
            {
                *link = s->registry_next;
                --r.count;
            }
        });

    delete s;
}

cc::rec::config cc::rec::config::create_default()
{
    config c;
#if CC_ASSERT_ENABLED
    c.overflow = rec::overflow_policy::backpressure;
    c.budget_bytes = 256 << 20;
#else
    c.overflow = rec::overflow_policy::drop;
    c.budget_bytes = 64 << 20;
#endif
    return c;
}

void cc::rec::initialize(cc::rec::config const& cfg)
{
    CC_ASSERT(!g_initialized.load(cc::memory_order_relaxed), "cc::rec::initialize called twice");

    g_config = cfg;
    g_cycles_per_second.store(measure_cycle_rate(), cc::memory_order_relaxed);
    impl::g_capture_core_id.store(cfg.capture_core_id, cc::memory_order_relaxed);

    auto const rate = g_cycles_per_second.load(cc::memory_order_relaxed);
    impl::g_drop_retry_cycles.store(rate > 0 ? u64(rate * cfg.drop_retry_secs) : 0, cc::memory_order_relaxed);

    auto* const pool = new rec::chunk_pool(cfg.chunk_bytes, cfg.budget_bytes, cfg.overflow);
    pool->refill(cfg.ready_chunks);
    impl::g_pool.store(pool, cc::memory_order_release);

    impl::g_system_generation.fetch_add(1, cc::memory_order_acq_rel);
    g_initialized.store(true, cc::memory_order_release);

    if (cfg.threaded && CC_HAS_THREADS != 0)
    {
        g_worker_stop.store(false, cc::memory_order_relaxed);
        g_worker_running.store(true, cc::memory_order_release);
        start_worker_thread(cfg.poll_interval_secs);
    }
}

void cc::rec::shutdown()
{
    if (!g_initialized.load(cc::memory_order_acquire))
        return;

    g_worker_stop.store(true, cc::memory_order_release);
    join_worker_thread();

    // The calling thread still owns a chunk of its own; seal it so the final drain sees everything.
    rec::seal_current_thread_chunk();
    g_processing.lock([](processing& p) { drain_all(p); });

    g_processing.lock(
        [](processing& p)
        {
            // Unregistering clears a slot in place rather than erasing it, so the vector staying non-empty says
            // nothing; what must hold is that no LIVE listener is left behind.
            for (auto const& r : p.listeners)
                CC_ASSERT(r.l == nullptr, "listeners must be unregistered before cc::rec::shutdown");

            p.listeners.clear();
            p.next_generation = 1;
        });

    // Invalidate every recording thread's cursor BEFORE the pool goes away.
    // A cursor with room left never reaches the cold path, so the generation check alone would not stop a thread from
    // writing into freed memory — this is the only thing that can.
    impl::for_each_thread_state(
        [](impl::thread_state& s)
        {
            if (s.tls == nullptr)
                return;

            // The chunk POINTERS go too, not just the cursors.
            // A thread that exits after this runs its seal handshake, and a `current` left pointing into the pool
            // about to be freed is a use-after-free with no cold path in between to catch it.
            s.tls->cur = nullptr;
            s.tls->end = nullptr;
            s.tls->current = nullptr;
            s.tls->alt_cur = nullptr;
            s.tls->alt_end = nullptr;
            s.tls->alt_current = nullptr;
            s.tls->state = nullptr;
            s.tls->alt_state = nullptr;
        });

    // Drop every chunk still queued, so the pool can be torn down.
    cc::vector<impl::thread_state*> all;
    impl::for_each_thread_state([&](impl::thread_state& s) { all.push_back(&s); });
    for (auto* s : all)
    {
        for (auto* c = s->consume_cursor; c != nullptr;)
        {
            auto* const next = c->next_in_thread.load(cc::memory_order_acquire);
            c->release_ref();
            c = next;
        }
        s->consume_cursor = nullptr;
        s->queue_head.store(nullptr, cc::memory_order_relaxed);
        s->produce_tail = nullptr;
        impl::reclaim_thread_state(s);
    }

    auto* const pool = impl::g_pool.exchange(nullptr, cc::memory_order_acq_rel);
    delete pool;

    // Every recording thread still holds a cursor into what was just freed; bumping the generation is how they are
    // told to forget it, since nothing here can reach another thread's thread-local state.
    impl::g_system_generation.fetch_add(1, cc::memory_order_acq_rel);
    g_initialized.store(false, cc::memory_order_release);
}

bool cc::rec::is_initialized()
{
    return g_initialized.load(cc::memory_order_acquire);
}

cc::rec::config const& cc::rec::current_config()
{
    return g_config;
}

f64 cc::rec::cycles_per_second()
{
    return g_cycles_per_second.load(cc::memory_order_relaxed);
}

cc::rec::listener_handle cc::rec::register_listener(cc::rec::listener& l)
{
    auto const handle = g_processing.lock(
        [&](processing& p)
        {
            // A cleared slot is reused rather than appended past.
            // A registration per test is a normal workload, and growing forever would leave every chunk dispatch
            // walking thousands of nulls — the generation is what keeps a stale handle from matching the reuser.
            auto index = p.listeners.size();
            for (isize i = 0; i < p.listeners.size(); ++i)
                if (p.listeners[i].l == nullptr)
                {
                    index = i;
                    break;
                }

            rec::listener_handle h;
            h._index = index;
            h._generation = p.next_generation++;

            if (index == p.listeners.size())
                p.listeners.push_back({.l = &l, .generation = h._generation});
            else
                p.listeners[index] = {.l = &l, .generation = h._generation};

            return h;
        });

    return handle;
}

bool cc::rec::is_listener_registered(cc::rec::listener_handle h)
{
    if (!h.is_valid())
        return false;

    return g_processing.lock(
        [&](processing& p)
        {
            if (h._index >= p.listeners.size())
                return false;

            // Both halves are load-bearing: the generation rules out a stale handle onto a REUSED slot, and the null
            // rules out one onto a slot that was cleared and not yet reused.
            return p.listeners[h._index].generation == h._generation && p.listeners[h._index].l != nullptr;
        });
}

void cc::rec::unregister_listener(cc::rec::listener_handle h)
{
    if (!h.is_valid())
        return;

    g_processing.lock(
        [&](processing& p)
        {
            if (h._index >= p.listeners.size())
                return;
            if (p.listeners[h._index].generation != h._generation)
                return;

            // Cleared in place rather than erased, so every other listener keeps the layer index it was given.
            p.listeners[h._index] = {};
        });
}

void cc::rec::flush_blocking()
{
    if (!g_initialized.load(cc::memory_order_acquire))
        return;

    g_processing.lock([](processing& p) { drain_all(p); });
}

cc::rec::system_stats cc::rec::stats()
{
    rec::system_stats s;
    s.threads = impl::thread_state_count();

    g_processing.lock(
        [&](processing const& p)
        {
            for (auto const& r : p.listeners)
                if (r.l != nullptr)
                    ++s.registered_listeners;
            s.chunks_processed = p.chunks_processed;
            s.events_processed = p.events_processed;
        });

    if (auto* const pool = impl::g_pool.load(cc::memory_order_acquire); pool != nullptr)
    {
        s.allocated_bytes = pool->allocated_bytes();
        s.ready_chunks = pool->ready_count();
        s.failed_acquires = pool->failed_acquires();
    }

    return s;
}
