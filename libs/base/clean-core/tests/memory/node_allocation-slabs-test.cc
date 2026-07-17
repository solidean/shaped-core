#include "node_allocation-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <thread>

// Slab-lifecycle tests for the frontend split. These use a *private* node_allocator bound to the real
// default resource (a fresh slab_info, isolated from the process-wide default_node_allocator() that other
// tests share) so they observe the real, shipped refill/ring/drain code from a clean slate. Slab lifecycle
// is asserted via distinct slab base addresses (node_slab_base_for_ptr): a new base == a new slab.

using namespace cc::primitive_defines;

namespace
{
// records `base` if not already present; returns true iff it was newly inserted
bool track_base(cc::vector<cc::byte*>& seen, cc::byte* base)
{
    for (auto* b : seen)
        if (b == base)
            return false;
    seen.push_back(base);
    return true;
}

template <class T>
cc::byte* base_of(cc::node_allocation<T> const& n)
{
    return cc::node_slab_base_for_ptr(reinterpret_cast<cc::byte*>(n.ptr), cc::node_class_index_for<T>());
}

// usable (non-metadata-blocked) slots per slab for T's class
template <class T>
int usable_slots()
{
    return cc::popcount(cc::node_seed_local_freemaps[cc::isize(cc::node_class_index_for<T>())]);
}

// number of slabs currently in an allocator's ring for a class (walks the public slab_base ring)
int ring_len(cc::node_allocator& alloc, cc::node_class_index idx)
{
    cc::byte* const head = alloc.slabs().slab_base[cc::isize(idx)];
    if (head == nullptr)
        return 0;
    int n = 0;
    cc::byte* cur = head;
    do
    {
        ++n;
        cur = cc::node_slab_next_for_base(cur);
    } while (cur != head);
    return n;
}
} // namespace

TEST("node_allocation - private allocator functional churn (both frontends)")
{
    cc::node_allocator alloc(cc::default_node_memory_resource);

    // churn a live set that spans several slabs; every value must round-trip through alloc+free cycles
    constexpr int live = 130;
    cc::vector<cc::node_allocation<T8B>> nodes;
    for (int i = 0; i < live; ++i)
        nodes.push_back({});

    for (int iter = 0; iter < 5000; ++iter)
    {
        int const i = iter % live;
        nodes[i] = cc::node_allocation<T8B>::create_from(alloc, u64(iter)); // frees the old slot i, then reallocs
        CHECK(nodes[i].is_valid());
        CHECK(nodes[i].ptr->value == u64(iter));
    }

    for (int i = 0; i < live; ++i)
        CHECK(nodes[i].is_valid());
}

TEST("node_allocation - bounded slab growth under churn (leak fix)")
{
    cc::node_allocator alloc(cc::default_node_memory_resource);
    auto const idx = cc::node_class_index_for<T8B>();
    int const usable = usable_slots<T8B>();

    // keep a live set spanning ~4 slabs and churn hard while counting distinct slabs ever touched.
    // the retaining ring keeps a handful of slabs and reuses them forever; the old leaky refill dropped
    // previous slabs and allocated a fresh one roughly every `usable` iterations -> hundreds of slabs.
    int const live = usable * 3 + 5;
    cc::vector<cc::node_allocation<T8B>> nodes;
    for (int i = 0; i < live; ++i)
        nodes.push_back({});
    cc::vector<cc::byte*> seen;

    for (int iter = 0; iter < 20000; ++iter)
    {
        int const i = iter % live;
        nodes[i] = cc::node_allocation<T8B>::create_from(alloc, u64(iter));
        track_base(seen, cc::node_slab_base_for_ptr(reinterpret_cast<cc::byte*>(nodes[i].ptr), idx));
    }

    CHECK(seen.size() <= 8);
}

TEST("node_allocation - multi-slab ring reuse")
{
    cc::node_allocator alloc(cc::default_node_memory_resource);
    int const usable = usable_slots<T8B>();

    // fill exactly 3 slabs; the head (3rd slab) ends up full
    cc::vector<cc::node_allocation<T8B>> nodes;
    for (int i = 0; i < usable * 3; ++i)
        nodes.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i)));

    cc::vector<cc::byte*> seen;
    for (auto const& n : nodes)
        track_base(seen, base_of(n));
    REQUIRE(seen.size() == 3);

    // free every slot of the first (non-head) slab back into it, on the owner thread
    cc::byte* const slab1 = base_of(nodes[0]);
    for (int i = 0; i < usable; ++i)
        nodes[i] = {};

    // the head is full, so the next `usable` allocations must walk the ring and reuse slab 1's freed slots
    for (int i = 0; i < usable; ++i)
    {
        auto n = cc::node_allocation<T8B>::create_from(alloc, u64(1000 + i));
        CHECK(base_of(n) == slab1); // reused slab 1, no new slab
    }
}

TEST("node_allocation - slab trim returns surplus fully-free slabs to backing")
{
    cc::node_allocator alloc(cc::default_node_memory_resource);
    auto const idx = cc::node_class_index_for<T8B>();
    int const usable = usable_slots<T8B>();

    // inflate to a high watermark (~6 slabs), then free everything so all those slabs become fully free.
    {
        cc::vector<cc::node_allocation<T8B>> peak;
        for (int i = 0; i < usable * 6; ++i)
            peak.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i)));
    }
    int const peak_ring = ring_len(alloc, idx);
    REQUIRE(peak_ring >= 6);

    // churn with small bursts that each spill just past one slab. this drives the cold path enough for the
    // gated trim sweep to run and hand the surplus fully-free slabs back to the backing resource.
    for (int round = 0; round < 1000; ++round)
    {
        cc::vector<cc::node_allocation<T8B>> burst;
        for (int i = 0; i < usable + 5; ++i)
            burst.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(round * 100000 + i)));
        for (auto const& n : burst)
            CHECK(n.is_valid());
    }

    // the ring shrank back toward the live working set (a burst needs ~2 slabs) instead of staying at the peak
    int const trimmed_ring = ring_len(alloc, idx);
    CHECK(trimmed_ring < peak_ring);
    CHECK(trimmed_ring <= 3);
}

#if CC_HAS_THREADS
TEST("node_allocation - remote drain reclaims cross-thread frees")
{
    cc::node_allocator alloc(cc::default_node_memory_resource);

    constexpr int batch = 200;
    cc::vector<cc::node_allocation<T8B>> nodes;
    for (int i = 0; i < batch; ++i)
        nodes.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i)));

    cc::vector<cc::byte*> seen;
    for (auto const& n : nodes)
        track_base(seen, base_of(n));

    // free the whole batch on ANOTHER thread -> each free routes into its slab's remote bitmap
    std::thread([&nodes] { nodes.clear(); }).join();

    // owner reallocates the same batch: local is empty everywhere, so the cold path must drain the remote
    // bitmaps to reclaim the slots -- otherwise it would allocate brand-new slabs (new bases).
    for (int i = 0; i < batch; ++i)
    {
        auto n = cc::node_allocation<T8B>::create_from(alloc, u64(1000 + i));
        CHECK(n.ptr->value == u64(1000 + i));
        CHECK(track_base(seen, base_of(n)) == false); // base already seen -> no new slab, drain worked
    }
}

TEST("node_allocation - cross-thread free then reuse (basic)")
{
    cc::node_allocator alloc(cc::default_node_memory_resource);

    auto a = cc::node_allocation<T8B>::create_from(alloc, u64(11));
    auto b = cc::node_allocation<T16B>::create_from(alloc, u64(22));
    auto c = cc::node_allocation<T64B>::create_from(alloc, u64(33));
    CHECK(a.ptr->value == 11);
    CHECK(b.ptr->a == 22);
    CHECK(c.ptr->data[0] == 33);

    // free all three on another thread (remote path: differing owner token)
    std::thread(
        [&]
        {
            a.reset();
            b.reset();
            c.reset();
        })
        .join();
    CHECK(!a.is_valid());
    CHECK(!b.is_valid());
    CHECK(!c.is_valid());

    // owner reallocates the same classes and they work
    auto a2 = cc::node_allocation<T8B>::create_from(alloc, u64(44));
    auto b2 = cc::node_allocation<T16B>::create_from(alloc, u64(55));
    CHECK(a2.ptr->value == 44);
    CHECK(b2.ptr->a == 55);
}

TEST("node_allocation - thread-exit reclaims fully-free slabs to backing")
{
    auto const before = cc::impl::node_orphan_slab_count();

    // a thread allocates a multi-slab batch, frees it ALL on itself, then exits. every slab is fully free
    // at the allocator's teardown -> returned to the backing resource, and NOTHING is orphaned.
    std::thread(
        []
        {
            cc::node_allocator worker(cc::default_node_memory_resource);
            cc::vector<cc::node_allocation<T8B>> nodes;
            for (int i = 0; i < usable_slots<T8B>() * 3; ++i)
                nodes.push_back(cc::node_allocation<T8B>::create_from(worker, u64(i)));
            nodes.clear(); // free everything on the owner thread before `worker` is destroyed
        })
        .join();

    CHECK(cc::impl::node_orphan_slab_count() == before); // orphan bins untouched -> slabs went to backing
}

TEST("node_allocation - abandoned slab is adopted by a later thread")
{
    auto const before = cc::impl::node_orphan_slab_count();
    int const usable = usable_slots<T8B>();

    // producer thread fills exactly one slab, hands every (live) node to a shared vector, then exits.
    // its slab still holds live nodes -> not fully free -> orphaned to the global bin on teardown.
    cc::vector<cc::node_allocation<T8B>> shared;
    cc::byte* producer_base = nullptr;
    std::thread(
        [&]
        {
            cc::node_allocator producer(cc::default_node_memory_resource);
            for (int i = 0; i < usable; ++i)
                shared.push_back(cc::node_allocation<T8B>::create_from(producer, u64(i)));
            producer_base = base_of(shared[0]);
        })
        .join();

    // the handed-off nodes are still valid after the producer thread is gone, and all in the one slab
    for (int i = 0; i < usable; ++i)
    {
        CHECK(base_of(shared[i]) == producer_base);
        CHECK(shared[i].ptr->value == u64(i));
    }
    CHECK(cc::impl::node_orphan_slab_count() == before + 1); // exactly one slab orphaned

    // free the whole batch on the consumer (main) thread -> routes to the orphaned slab's remote bitmap
    // (the owner token is the dead producer's, never this thread's)
    for (auto& n : shared)
        n = {};

    // a fresh consumer allocator refills: it must ADOPT the orphan (draining its remote frees) and reuse
    // the producer's slab rather than mallocing a new one.
    cc::node_allocator consumer(cc::default_node_memory_resource);
    cc::vector<cc::node_allocation<T8B>> consumer_nodes;
    for (int i = 0; i < usable; ++i)
    {
        consumer_nodes.push_back(cc::node_allocation<T8B>::create_from(consumer, u64(7000 + i)));
        CHECK(base_of(consumer_nodes.back()) == producer_base); // adopted slab, no new slab
    }
    for (int i = 0; i < usable; ++i)
        CHECK(consumer_nodes[i].ptr->value == u64(7000 + i));

    CHECK(cc::impl::node_orphan_slab_count() == before); // the orphan was adopted -> bin drained
}
#endif
