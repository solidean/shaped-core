#include "node_allocation-test-types.hh"

#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <thread>

// The per-thread default node allocator: hydration, set/get, and scoped overrides.
//
// default_node_allocator() resolves the default resource's allocator once per thread and caches the
// result in a TLS slot so the alloc fast path can inline. That slot is authoritative, not a shadow
// cache: an override must be visible to every later allocation, including on a thread that has
// already allocated. An earlier attempt at this caching froze the allocator permanently, so these
// tests pin the "still repointable" half of the contract.
//
// Which allocator served a node is observed via its slab base (node_slab_base_for_ptr): a node from
// the override lives in a slab the override owns, never in the system allocator's.

using namespace cc::primitive_defines;

namespace
{
template <class T>
cc::byte* base_of(cc::node_allocation<T> const& n)
{
    return cc::node_slab_base_for_ptr(reinterpret_cast<cc::byte*>(n.ptr), cc::node_class_index_for<T>());
}

template <class T>
cc::byte* ring_head(cc::node_allocator const& alloc)
{
    return alloc.slabs().slab_base[cc::isize(cc::node_class_index_for<T>())];
}
} // namespace

TEST("node_allocation - default allocator hydrates once per thread")
{
    auto& a = cc::default_node_allocator();
    auto& b = cc::default_node_allocator();

    CHECK(&a == &b);
    // hydration installs into the slot, so the raw getter now agrees with the resolving one
    CHECK(cc::get_default_node_allocator() == &a);
}

TEST("node_allocation - thread default is repointable after allocating")
{
    // hydrate first: repointing a thread that has ALREADY allocated is the case the reverted
    // resolve-once cache got wrong, so it is the one worth pinning.
    auto warm = cc::node_allocation<T8B>::create_from(cc::default_node_allocator(), u64(1));
    CHECK(warm.is_valid());

    auto* const system_default = cc::get_default_node_allocator();
    CHECK(system_default != nullptr);

    cc::node_allocator over(cc::default_node_memory_resource);

    {
        cc::scoped_default_node_allocator guard(&over);

        CHECK(cc::get_default_node_allocator() == &over);
        CHECK(&cc::default_node_allocator() == &over); // resolving must not re-hydrate over the override

        auto n = cc::node_allocation<T8B>::create_from(cc::default_node_allocator(), u64(7));
        CHECK(n.ptr->value == u64(7));
        CHECK(base_of(n) == ring_head<T8B>(over));
        CHECK(base_of(n) != ring_head<T8B>(*system_default));
    } // n is freed before `over` dies -- the allocator must outlive its nodes

    CHECK(cc::get_default_node_allocator() == system_default);
}

TEST("node_allocation - scoped overrides nest")
{
    (void)cc::default_node_allocator(); // hydrate so the outermost restore target is not null
    auto* const system_default = cc::get_default_node_allocator();

    cc::node_allocator outer(cc::default_node_memory_resource);
    cc::node_allocator inner(cc::default_node_memory_resource);

    {
        cc::scoped_default_node_allocator g_outer(&outer);
        CHECK(cc::get_default_node_allocator() == &outer);
        {
            cc::scoped_default_node_allocator g_inner(&inner);
            CHECK(cc::get_default_node_allocator() == &inner);
        }
        // restores to the OUTER override, not to null and not to the system default: a guard that
        // reset unconditionally would silently drop g_outer here
        CHECK(cc::get_default_node_allocator() == &outer);
    }

    CHECK(cc::get_default_node_allocator() == system_default);
}

TEST("node_allocation - setting the default to null re-resolves")
{
    (void)cc::default_node_allocator();
    auto* const system_default = cc::get_default_node_allocator();

    cc::node_allocator over(cc::default_node_memory_resource);
    cc::set_default_node_allocator(&over);
    CHECK(cc::get_default_node_allocator() == &over);

    cc::set_default_node_allocator(nullptr);
    CHECK(cc::get_default_node_allocator() == nullptr);     // reset, not yet hydrated
    CHECK(&cc::default_node_allocator() == system_default); // next use re-resolves the system one
    CHECK(cc::get_default_node_allocator() == system_default);
}

#if CC_HAS_THREADS
TEST("node_allocation - a fresh thread starts unhydrated and restores to null")
{
    // observations are recorded on the worker and asserted here: nexus tracks checks per-thread, so
    // a CHECK inside the lambda would not count towards this test.
    bool starts_null = false;
    bool value_ok = false;
    bool served_by_local = false;
    bool restored_to_null = false;

    std::thread(
        [&]
        {
            // a thread that never allocated has an empty slot; nothing has resolved for it yet
            starts_null = cc::get_default_node_allocator() == nullptr;

            cc::node_allocator local(cc::default_node_memory_resource);
            {
                cc::scoped_default_node_allocator guard(&local);
                auto n = cc::node_allocation<T8B>::create_from(cc::default_node_allocator(), u64(3));
                value_ok = n.ptr->value == u64(3);
                served_by_local = base_of(n) == ring_head<T8B>(local);
            }

            // the guard saved null and must restore exactly that -- the thread is still unhydrated
            restored_to_null = cc::get_default_node_allocator() == nullptr;
        })
        .join();

    CHECK(starts_null);
    CHECK(value_ok);
    CHECK(served_by_local);
    CHECK(restored_to_null);
}

TEST("node_allocation - each thread has its own default")
{
    auto* const main_default = &cc::default_node_allocator();
    cc::node_allocator* worker_default = nullptr;

    std::thread([&] { worker_default = &cc::default_node_allocator(); }).join();

    CHECK(worker_default != nullptr);
    CHECK(worker_default != main_default);                   // the system resource segregates allocators per thread
    CHECK(cc::get_default_node_allocator() == main_default); // the worker never touched our slot
}
#endif
