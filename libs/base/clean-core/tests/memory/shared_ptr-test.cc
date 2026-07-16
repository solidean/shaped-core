#include <clean-core/memory/shared_ptr.hh>
#include <nexus/test.hh>

#include <atomic>
#include <thread>

// Isolated tests for cc::shared_ptr / cc::weak_ptr. Two layouts are exercised: the default trailing-control
// traits (cc::default_shared_traits, used by shared_ptr<T> with no Traits) and a custom INTRUSIVE traits whose
// counts are members of the node — the shape cc::async will use. The intrusive test types deliberately tear
// down only their payload in destroy_object and leave the count members alive until free_storage, because the
// weak count must outlive the object's destruction (reading a destroyed atomic would be UB).

namespace
{
// --- a payload type for the DEFAULT (trailing-control) traits ------------------------------------------------

struct tracked
{
    static inline int live = 0;
    static inline int destroyed = 0;
    static void reset() { live = destroyed = 0; }

    int value;
    explicit tracked(int v) : value(v) { ++live; }
    ~tracked()
    {
        --live;
        ++destroyed;
    }
};

// --- an INTRUSIVE base/derived pair: counts are members; one Traits keyed on the base ------------------------

struct node_base
{
    static inline int payload_torn = 0;
    static inline int freed = 0;
    static void reset() { payload_torn = freed = 0; }

    std::atomic<cc::u32> strong{0};
    std::atomic<cc::u32> weak{0};
    cc::node_class_index cls{}; // concrete size class, stashed so free frees the right (derived) size

    virtual void teardown_payload() = 0; // destroy the payload only; leaves the counts alive for weak refs

protected:
    ~node_base() = default; // never invoked: nodes are reclaimed by free_storage, not delete
};

struct node_derived : node_base
{
    cc::u64 padding[6] = {}; // distinctly larger size class than the base
    int value;

    explicit node_derived(int v) : value(v) { cls = cc::node_class_index_for<node_derived>(); }
    void teardown_payload() override { ++payload_torn; } // trivial payload -> nothing to actually destroy
};

struct node_traits
{
    static constexpr bool supports_weak = true;

    // intrusive: the node IS the object, so no extra control storage
    static constexpr cc::isize node_size(cc::isize psize, cc::isize) { return psize; }
    static constexpr cc::isize node_align(cc::isize palign) { return palign; }

    static void init_control(node_base* p)
    {
        p->strong.store(1, std::memory_order_relaxed);
        p->weak.store(1, std::memory_order_relaxed);
    }
    static void inc_strong(node_base* p) { p->strong.fetch_add(1, std::memory_order_relaxed); }
    static cc::shared_release release_strong(node_base* p)
    {
        return {p->strong.fetch_sub(1, std::memory_order_acq_rel) == 1, false};
    }
    static void inc_weak(node_base* p) { p->weak.fetch_add(1, std::memory_order_relaxed); }
    static bool release_weak(node_base* p) { return p->weak.fetch_sub(1, std::memory_order_acq_rel) == 1; }
    static bool try_lock_strong(node_base* p)
    {
        cc::u32 cur = p->strong.load(std::memory_order_relaxed);
        while (cur != 0)
            if (p->strong.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                return true;
        return false;
    }
    static void destroy_object(node_base* p) { p->teardown_payload(); } // leaves the count members alive
    static void free_storage(node_base* p)
    {
        ++node_base::freed;
        cc::node_allocation_free(reinterpret_cast<cc::byte*>(p), p->cls); // stashed concrete class, not the base's
    }
};

using node_ptr = cc::shared_ptr<node_base, node_traits>;
using node_weak = cc::weak_ptr<node_base, node_traits>;

// --- a strong-only intrusive type ----------------------------------------------------------------------------

struct only_strong
{
    static inline int torn = 0;
    static inline int freed = 0;
    static void reset() { torn = freed = 0; }

    std::atomic<cc::u32> strong{0};
    int value;
    explicit only_strong(int v) : value(v) {}
};

struct only_strong_traits
{
    static constexpr bool supports_weak = false;
    static constexpr cc::isize node_size(cc::isize psize, cc::isize) { return psize; }
    static constexpr cc::isize node_align(cc::isize palign) { return palign; }
    static void init_control(only_strong* p) { p->strong.store(1, std::memory_order_relaxed); }
    static void inc_strong(only_strong* p) { p->strong.fetch_add(1, std::memory_order_relaxed); }
    static cc::shared_release release_strong(only_strong* p)
    {
        bool const last = p->strong.fetch_sub(1, std::memory_order_acq_rel) == 1;
        return {last, last}; // no weak count to wait on: destroy and free together
    }
    static void destroy_object(only_strong* p) { ++only_strong::torn; }
    static void free_storage(only_strong* p)
    {
        ++only_strong::freed;
        cc::node_allocation_free(reinterpret_cast<cc::byte*>(p), cc::node_class_index_for<only_strong>());
    }
};
} // namespace

// ============================================================================
// default (trailing-control) traits
// ============================================================================

TEST("shared_ptr - default traits: make, copy/move keep alive, single destroy")
{
    tracked::reset();
    {
        auto p = cc::make_shared<tracked>(42); // Traits defaulted to default_shared_traits<tracked>
        CHECK(p.is_valid());
        CHECK(p->value == 42);
        CHECK(tracked::live == 1);

        {
            auto q = p; // copy: still one object, two owners
            CHECK(q.get() == p.get());
        }
        CHECK(tracked::destroyed == 0); // dropping the copy did not destroy

        auto m = cc::move(p);
        CHECK(!p.is_valid());
        CHECK(m->value == 42);
    }
    CHECK(tracked::live == 0);
    CHECK(tracked::destroyed == 1);
}

TEST("shared_ptr - default traits: weak lock while alive, expired after last strong")
{
    tracked::reset();
    cc::weak_ptr<tracked> w;
    {
        auto p = cc::make_shared<tracked>(7);
        w = p;
        auto l = w.lock();
        REQUIRE(l.is_valid());
        CHECK(l->value == 7);
    } // both strong owners gone -> destroyed once; storage lingers for the surviving weak

    CHECK(tracked::destroyed == 1);
    CHECK(!w.lock().is_valid()); // expired, no resurrection
    w = nullptr;                 // last weak -> storage freed (no crash / double free)
}

TEST("shared_ptr - default traits: release/adopt round trip keeps the sole-owner path intact")
{
    // node_traits (below) pins count-neutrality on hand-rolled counts; this pins the same round trip through the
    // DEFAULT traits, which are the ones built on cc::fused_refcount. Since release/adopt never touch the
    // counter, the sole-owner (1,1) no-RMW fast path must still fire on the adopter's drop.
    tracked::reset();
    {
        auto p = cc::make_shared<tracked>(42);
        tracked* const raw = p.release();
        CHECK(tracked::live == 1); // released, not destroyed
        CHECK(tracked::destroyed == 0);

        auto back = cc::shared_ptr<tracked>::adopt(raw);
        CHECK(back->value == 42);
    }
    CHECK(tracked::live == 0);
    CHECK(tracked::destroyed == 1); // exactly once, via the adopted count
}

TEST("shared_ptr - default traits: plain value type (shared_ptr<int>)")
{
    auto p = cc::make_shared<int>(123);
    REQUIRE(p.is_valid());
    CHECK(*p == 123);
    auto q = p;
    *q = 7;
    CHECK(*p == 7); // same object
}

// The fused control is 8-aligned, so control_offset rounds sizeof(T) up to 8 and the node grows by up to 4 B
// over a split-u32 control. That never crosses a size class: the node goes from 8m+4 to 8m+8 bytes, and the
// only power of two in [8m+4, 8m+8) would have to be 8m+8 itself. shared_ptr<int> is the tight case — a 16 B
// node in the 16 B class, where a split control gave a 12 B node in the same class.
namespace
{
using int_traits = cc::default_shared_traits<int>;
static_assert(int_traits::node_size(sizeof(int), alignof(int)) == 16);
static_assert(int_traits::node_align(alignof(int)) == 8);
static_assert(cc::node_class_index_from_size_and_align(int_traits::node_size(sizeof(int), alignof(int)),
                                                       int_traits::node_align(alignof(int)))
              == cc::node_class_index(4)); // 2^4 == 16 B class, unchanged by the fusion
} // namespace

// ============================================================================
// intrusive traits (the async shape): counts as members, weak outlives destroy
// ============================================================================

TEST("shared_ptr - intrusive: refcounts, single payload teardown + free")
{
    node_base::reset();
    {
        auto p = cc::make_shared<node_derived, node_traits>(5);
        CHECK(p->value == 5);
        CHECK(p->strong.load() == 1);
        CHECK(p->weak.load() == 1);

        {
            auto q = p;
            CHECK(p->strong.load() == 2);
        }
        CHECK(p->strong.load() == 1);
        CHECK(node_base::payload_torn == 0);
    }
    CHECK(node_base::payload_torn == 1);
    CHECK(node_base::freed == 1);
}

TEST("shared_ptr - intrusive: weak outlives the object, frees on last weak")
{
    node_base::reset();
    node_weak w;
    {
        auto p = cc::make_shared<node_derived, node_traits>(7);
        w = p;
        CHECK(p->weak.load() == 2);

        auto l = w.lock();
        REQUIRE(l.is_valid());
        CHECK(p->strong.load() == 2);
    } // last strong gone -> payload torn down, counts still alive for w

    CHECK(node_base::payload_torn == 1);
    CHECK(node_base::freed == 0); // NOT freed yet: w holds a weak count
    CHECK(!w.lock().is_valid());  // strong is 0 -> upgrade fails
    w = nullptr;
    CHECK(node_base::freed == 1);
}

TEST("shared_ptr - intrusive: upcast to base and polymorphic free via stashed class")
{
    node_base::reset();
    {
        auto d = cc::make_shared<node_derived, node_traits>(99);

        node_ptr b = d; // upcast copy shares the same control
        CHECK(b.get() == static_cast<node_base*>(d.get()));
        CHECK(d->strong.load() == 2);

        node_weak wb = d; // weak on the base, from a derived handle
        auto l = wb.lock();
        REQUIRE(l.is_valid());
        CHECK(l.get() == b.get());
    }
    CHECK(node_base::payload_torn == 1);
    CHECK(node_base::freed == 1); // freed exactly once, with the derived size class (via stashed cls)
}

TEST("shared_ptr - intrusive: from_alive mints an extra strong handle")
{
    node_base::reset();
    {
        auto p = cc::make_shared<node_derived, node_traits>(3);
        node_base* raw = p.get();

        auto p2 = node_ptr::from_alive(raw); // self-recovery: raw is known alive (p holds a strong)
        CHECK(p->strong.load() == 2);
        CHECK(p2.get() == raw);
    }
    CHECK(node_base::payload_torn == 1);
    CHECK(node_base::freed == 1);
}

// --- release()/adopt(): moving a strong count into and out of hand-rolled storage ---------------------------
// The twin of weak_ptr's pair. These exist so a count can live somewhere shared_ptr itself cannot (a lock-free
// deque of raw node pointers, a tagged word), so the property that matters is COUNT-NEUTRALITY: the round trip
// must not touch the refcount at all. Behavior alone would pass even if it did an inc/dec pair, hence the
// explicit count assertions.

TEST("shared_ptr - intrusive: release/adopt round trip is count-neutral")
{
    node_base::reset();
    {
        auto p = cc::make_shared<node_derived, node_traits>(7);
        node_base* const raw = p.get();
        CHECK(raw->strong.load() == 1);
        CHECK(raw->weak.load() == 1);

        node_base* const handed_off = p.release(); // hand the count to raw storage: no dec, nothing destroyed
        CHECK(handed_off == raw);
        CHECK(p == nullptr); // the released handle is empty and owes nothing
        CHECK(node_base::payload_torn == 0);
        CHECK(raw->strong.load() == 1); // still exactly one strong: the count MOVED, it was not dropped
        CHECK(raw->weak.load() == 1);

        auto back = node_ptr::adopt(handed_off); // take it back: no inc either
        CHECK(back.get() == raw);
        CHECK(raw->strong.load() == 1);
        CHECK(raw->weak.load() == 1);
        CHECK(node_base::payload_torn == 0);
    }
    CHECK(node_base::payload_torn == 1); // the adopted count still fires teardown exactly once
    CHECK(node_base::freed == 1);
}

TEST("shared_ptr - intrusive: an adopted count is the one that destroys")
{
    node_base::reset();
    node_base* raw = nullptr;
    node_weak w;
    {
        auto p = cc::make_shared<node_derived, node_traits>(1);
        raw = p.get();
        w = p; // a weak ref, so storage outlives the object

        raw = p.release();
        CHECK(w.lock().is_valid()); // still alive: the count sits in raw storage, but it is still a count
    }
    CHECK(node_base::payload_torn == 0); // p going out of scope destroyed nothing: it no longer owned anything

    {
        auto owner = node_ptr::adopt(raw);
        CHECK(owner->strong.load() == 1);
    }
    CHECK(node_base::payload_torn == 1); // dropping the adopter is what tore it down
    CHECK(node_base::freed == 0);        // ... but the weak still pins the storage
    CHECK(!w.lock().is_valid());         // and can no longer be upgraded

    w = nullptr;
    CHECK(node_base::freed == 1);
}

TEST("shared_ptr - release/adopt on an empty handle is a safe no-op")
{
    node_base::reset();
    {
        node_ptr empty;
        CHECK(empty.release() == nullptr);

        auto adopted = node_ptr::adopt(nullptr);
        CHECK(adopted == nullptr); // and dropping it must not touch a null
    }
    CHECK(node_base::payload_torn == 0);
    CHECK(node_base::freed == 0);
}

TEST("shared_ptr - intrusive: release from a derived handle, adopt into the base")
{
    // The exact shape the async pool's deque uses: shared_async<T> is released to a raw async_node_base*, and
    // the count is later adopted back through the base-typed handle.
    node_base::reset();
    {
        auto d = cc::make_shared<node_derived, node_traits>(5);
        node_ptr b = d; // upcast: same control, now 2 strong
        CHECK(d->strong.load() == 2);

        node_base* const raw = b.release();
        CHECK(d->strong.load() == 2); // the released count is still held, just not by a handle

        auto back = node_ptr::adopt(raw);
        CHECK(d->strong.load() == 2);
        CHECK(back.get() == static_cast<node_base*>(d.get()));
    }
    CHECK(node_base::payload_torn == 1);
    CHECK(node_base::freed == 1); // freed once, with the derived size class
}

TEST("shared_ptr - strong-only traits: no weak, destroy + free together")
{
    only_strong::reset();
    {
        auto p = cc::make_shared<only_strong, only_strong_traits>(3);
        CHECK(p->value == 3);
        CHECK(p->strong.load() == 1);
        auto q = p;
        CHECK(p->strong.load() == 2);
    }
    CHECK(only_strong::torn == 1);
    CHECK(only_strong::freed == 1);
}

// ============================================================================
// fused_refcount — white-box on the half arithmetic
// ============================================================================

// Windows has no sanitizer preset, so the half arithmetic is pinned directly rather than only through the
// handles that use it. strong lives in the high 32 bits, weak in the low 32.
namespace
{
cc::u64 strong_of(std::atomic<cc::u64> const& c)
{
    return c.load() >> 32;
}
cc::u64 weak_of(std::atomic<cc::u64> const& c)
{
    return c.load() & 0xFFFF'FFFF;
}
} // namespace

TEST("fused_refcount - init, inc, and the half layout")
{
    using fr = cc::fused_refcount;
    static_assert(fr::sole_owner == ((cc::u64(1) << 32) | 1));

    std::atomic<cc::u64> c{0};
    fr::init(c);
    CHECK(c.load() == fr::sole_owner);
    CHECK(strong_of(c) == 1);
    CHECK(weak_of(c) == 1); // the strong owners' collective weak count

    fr::inc_strong(c);
    CHECK(strong_of(c) == 2);
    CHECK(weak_of(c) == 1); // strong owners SHARE one weak count — inc_strong must not touch the low half

    fr::inc_weak(c);
    CHECK(strong_of(c) == 2);
    CHECK(weak_of(c) == 2);
}

TEST("fused_refcount - release_strong reports destroy/free per the protocol")
{
    using fr = cc::fused_refcount;
    std::atomic<cc::u64> c{0};

    // (2,2): not the last strong -> nothing to do.
    fr::init(c);
    fr::inc_strong(c);
    fr::inc_weak(c);
    auto r = fr::release_strong(c);
    CHECK(!r.destroy);
    CHECK(!r.free);
    CHECK(strong_of(c) == 1);
    CHECK(weak_of(c) == 2); // release_strong drops only the high half

    // (1,2): last strong, but a weak_ptr survives -> destroy, do NOT free; the weak drop frees later.
    r = fr::release_strong(c);
    CHECK(r.destroy);
    CHECK(!r.free); // a weak ref outlives us: free is not ours to call
    CHECK(strong_of(c) == 0);
    CHECK(!fr::release_weak(c)); // the collective weak, released after destroy_object — one weak_ptr left
    CHECK(fr::release_weak(c));  // that last weak_ptr frees
    CHECK(c.load() == 0);

    // (1,1): sole owner -> destroy and free in one, no RMW.
    fr::init(c);
    r = fr::release_strong(c);
    CHECK(r.destroy);
    CHECK(r.free);
    CHECK(c.load() == fr::sole_owner); // the fast path deliberately leaves the counts untouched
}

// ============================================================================
// the destroy-before-free ordering, under an actual race
// ============================================================================

// The end-to-end companion to the fused_refcount white-box tests: the last strong drop and a weak drop, on two
// threads, must still destroy once and free once — and never free while destroy_object is running. teardown
// holds a window open and free_storage reports if a free lands inside it. Statics, not members: a node freed
// mid-teardown must not be read to detect that it was.
//
// The deterministic guard against the rejected "each strong owns its own weak" design is the white-box
// weak_of(c) == 2 check above, not this test — a thread race only samples the window.
namespace
{
struct race_node
{
    static inline std::atomic<int> torn{0};
    static inline std::atomic<int> freed{0};
    static inline std::atomic<bool> tearing{false};
    static inline std::atomic<int> freed_during_teardown{0};
    static void reset()
    {
        torn = 0;
        freed = 0;
        tearing = false;
        freed_during_teardown = 0;
    }

    std::atomic<cc::u64> counts{0};

    void teardown_payload()
    {
        tearing.store(true, std::memory_order_release);
        for (int i = 0; i < 400; ++i) // hold the window open so a racing weak drop can hit it
            _sink.fetch_add(1, std::memory_order_relaxed);
        tearing.store(false, std::memory_order_release);
        torn.fetch_add(1, std::memory_order_relaxed);
    }

private:
    static inline std::atomic<int> _sink{0};
};

struct race_traits
{
    static constexpr bool supports_weak = true;
    static constexpr cc::isize node_size(cc::isize psize, cc::isize) { return psize; }
    static constexpr cc::isize node_align(cc::isize palign) { return palign; }

    static void init_control(race_node* p) { cc::fused_refcount::init(p->counts); }
    static void inc_strong(race_node* p) { cc::fused_refcount::inc_strong(p->counts); }
    static cc::shared_release release_strong(race_node* p) { return cc::fused_refcount::release_strong(p->counts); }
    static void inc_weak(race_node* p) { cc::fused_refcount::inc_weak(p->counts); }
    static bool release_weak(race_node* p) { return cc::fused_refcount::release_weak(p->counts); }
    static bool try_lock_strong(race_node* p) { return cc::fused_refcount::try_lock_strong(p->counts); }
    static void destroy_object(race_node* p) { p->teardown_payload(); }
    static void free_storage(race_node* p)
    {
        if (race_node::tearing.load(std::memory_order_acquire))
            race_node::freed_during_teardown.fetch_add(1, std::memory_order_relaxed);
        race_node::freed.fetch_add(1, std::memory_order_relaxed);
        cc::node_allocation_free(reinterpret_cast<cc::byte*>(p), cc::node_class_index_for<race_node>());
    }
};
} // namespace

TEST("shared_ptr - a racing weak drop never frees while destroy_object runs")
{
    for (int it = 0; it < 200; ++it)
    {
        race_node::reset();

        auto p = cc::make_shared<race_node, race_traits>();
        cc::weak_ptr<race_node, race_traits> w = p; // a weak ref exists -> the sole-owner fast path must not fire

        // Spawning costs far more than the teardown window, so the worker must already be spinning before the
        // strong drop starts — otherwise the two never overlap and the test samples nothing.
        std::atomic<bool> spinning{false};
        std::atomic<bool> go{false};
        std::thread t(
            [&]
            {
                spinning.store(true, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                {
                }
                w.reset(); // races the last strong drop below
            });
        while (!spinning.load(std::memory_order_acquire))
        {
        }

        go.store(true, std::memory_order_release);
        p.reset(); // last strong: destroy_object, THEN the collective weak
        t.join();

        REQUIRE(race_node::torn.load() == 1);
        REQUIRE(race_node::freed.load() == 1); // exactly one free, whoever got there last
        REQUIRE(race_node::freed_during_teardown.load() == 0);
    }
}

TEST("fused_refcount - try_lock_strong follows the high half only")
{
    using fr = cc::fused_refcount;
    std::atomic<cc::u64> c{0};
    fr::init(c);

    CHECK(fr::try_lock_strong(c)); // strong > 0 -> upgrade succeeds
    CHECK(strong_of(c) == 2);
    CHECK(weak_of(c) == 1); // the new strong owner shares the existing collective weak

    fr::inc_weak(c); // a weak_ptr, so the counts never read (1,1) and the fast path stays out of the way
    (void)fr::release_strong(c);
    CHECK(fr::release_strong(c).destroy); // strong now 0, storage still alive for the weak ref
    CHECK(strong_of(c) == 0);
    CHECK(weak_of(c) == 2); // the collective weak is still held: reset() releases it after destroy_object

    CHECK(!fr::try_lock_strong(c)); // strong == 0 -> expired, however much weak churns
    CHECK(strong_of(c) == 0);
}
