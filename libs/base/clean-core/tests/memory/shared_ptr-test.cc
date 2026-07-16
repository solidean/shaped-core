#include <clean-core/memory/shared_ptr.hh>
#include <nexus/test.hh>

#include <atomic>

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
