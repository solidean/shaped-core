#include "node_allocation-test-types.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <array>

// Coverage for the two non-templated-on-a-single-T handles:
//   any_node_allocation           - type-erased (void* + deleter + class index)
//   poly_node_allocation<T, Traits> - class index recovered dynamically at destruction (polymorphism)

using namespace cc::primitive_defines;

// ------------------------------------------------------------------------------------------------
// any_node_allocation
// ------------------------------------------------------------------------------------------------

TEST("any_node_allocation - default constructed is invalid")
{
    cc::any_node_allocation any;
    CHECK(!any.is_valid());
    CHECK(!any);
    CHECK(any.ptr == nullptr);
    CHECK(any.deleter == nullptr);
}

TEST("any_node_allocation - construct from node_allocation erases the type")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("trivially destructible payload keeps a null deleter")
    {
        auto node = cc::node_allocation<T8B>::create_from(alloc, u64(42));
        void* const original_ptr = node.ptr;

        cc::any_node_allocation any = cc::move(node);

        // source handle was emptied, ownership moved into the type-erased handle
        CHECK(!node.is_valid());
        CHECK(any.is_valid());
        CHECK(static_cast<bool>(any));
        CHECK(any.ptr == original_ptr);
        CHECK(any.class_index == cc::node_class_index_for<T8B>());
        CHECK(any.deleter == nullptr); // T8B is trivially destructible
        CHECK(reinterpret_cast<T8B*>(any.ptr)->value == u64(42));
    }

    SECTION("non-trivial payload carries a deleter")
    {
        TrackedDtor::reset_counters();
        auto node = cc::node_allocation<TrackedDtor>::create_from(alloc, 7);

        cc::any_node_allocation any = cc::move(node);
        CHECK(any.deleter != nullptr);
        CHECK(any.class_index == cc::node_class_index_for<TrackedDtor>());
        CHECK(reinterpret_cast<TrackedDtor*>(any.ptr)->value == 7);
    }
}

TEST("any_node_allocation - destructor runs the deleter exactly once")
{
    auto& alloc = cc::default_node_allocator();

    TrackedDtor::reset_counters();
    {
        cc::any_node_allocation any = cc::node_allocation<TrackedDtor>::create_from(alloc, 1);
        CHECK(TrackedDtor::dtor_counter == 0);
    }
    CHECK(TrackedDtor::dtor_counter == 1);
}

TEST("any_node_allocation - reset destroys and empties, safe on empty")
{
    auto& alloc = cc::default_node_allocator();

    TrackedDtor::reset_counters();
    cc::any_node_allocation any = cc::node_allocation<TrackedDtor>::create_from(alloc, 5);

    any.reset();
    CHECK(TrackedDtor::dtor_counter == 1);
    CHECK(!any.is_valid());
    CHECK(any.ptr == nullptr);

    any.reset(); // already empty -> no-op, no extra dtor
    CHECK(TrackedDtor::dtor_counter == 1);
}

TEST("any_node_allocation - move construction transfers ownership")
{
    auto& alloc = cc::default_node_allocator();

    TrackedDtor::reset_counters();
    cc::any_node_allocation any1 = cc::node_allocation<TrackedDtor>::create_from(alloc, 9);
    void* const original_ptr = any1.ptr;
    auto const original_class = any1.class_index;

    cc::any_node_allocation any2 = cc::move(any1);

    CHECK(!any1.is_valid());
    CHECK(any1.deleter == nullptr); // deleter moved out too
    CHECK(any2.is_valid());
    CHECK(any2.ptr == original_ptr);
    CHECK(any2.class_index == original_class);
    CHECK(any2.deleter != nullptr);
    CHECK(TrackedDtor::dtor_counter == 0); // nothing destroyed by the move
    CHECK(reinterpret_cast<TrackedDtor*>(any2.ptr)->value == 9);
}

TEST("any_node_allocation - move assignment frees the previous payload")
{
    auto& alloc = cc::default_node_allocator();

    TrackedDtor::reset_counters();
    cc::any_node_allocation any1 = cc::node_allocation<TrackedDtor>::create_from(alloc, 1);
    cc::any_node_allocation any2 = cc::node_allocation<TrackedDtor>::create_from(alloc, 2);
    CHECK(TrackedDtor::dtor_counter == 0);

    any2 = cc::move(any1);

    // any2's original payload (value 2) is destroyed; any1's payload (value 1) is adopted
    CHECK(TrackedDtor::dtor_counter == 1);
    CHECK(!any1.is_valid());
    CHECK(any2.is_valid());
    CHECK(reinterpret_cast<TrackedDtor*>(any2.ptr)->value == 1);
}

TEST("any_node_allocation - class index round-trips through free across classes")
{
    auto& alloc = cc::default_node_allocator();

    // Slots alternate between two very different size classes. Because free derives the slab from the
    // stored class index, a wrong index would compute the wrong slab base and trip the double-free assert
    // or corrupt the bitmap. Even-idx slots hold T64B, odd-idx slots hold T1B; each is read back per iter.
    std::array<cc::any_node_allocation, 32> slots;

    for (int iter = 0; iter < 4000; ++iter)
    {
        int const idx = iter % 32;
        if (idx % 2 == 0)
        {
            slots[idx] = cc::node_allocation<T64B>::create_from(alloc, u64(iter));
            CHECK(slots[idx].class_index == cc::node_class_index_for<T64B>());
            CHECK(reinterpret_cast<T64B*>(slots[idx].ptr)->data[0] == u64(iter));
        }
        else
        {
            slots[idx] = cc::node_allocation<T1B>::create_from(alloc, u8(iter));
            CHECK(slots[idx].class_index == cc::node_class_index_for<T1B>());
            CHECK(reinterpret_cast<T1B*>(slots[idx].ptr)->value == u8(iter));
        }
    }
}

// ------------------------------------------------------------------------------------------------
// poly_node_allocation
// ------------------------------------------------------------------------------------------------

namespace
{
int poly_small_dtors = 0;
int poly_large_dtors = 0;

void reset_poly_counters()
{
    poly_small_dtors = 0;
    poly_large_dtors = 0;
}

// Polymorphic hierarchy whose leaf types land in *different* node size classes: the handle is typed on
// the base, but each leaf is allocated (and must be freed) in its own class.
struct PolyBase
{
    virtual cc::node_class_index which_class() const = 0;
    virtual int value() const = 0;
    virtual ~PolyBase() = default;
};

struct PolySmall : PolyBase
{
    int v = 0;
    explicit PolySmall(int x) : v(x) {}
    cc::node_class_index which_class() const override { return cc::node_class_index_for<PolySmall>(); }
    int value() const override { return v; }
    ~PolySmall() override { ++poly_small_dtors; }
};

struct PolyLarge : PolyBase
{
    u64 data[16] = {};
    explicit PolyLarge(int x)
    {
        for (auto& d : data)
            d = u64(x);
    }
    cc::node_class_index which_class() const override { return cc::node_class_index_for<PolyLarge>(); }
    int value() const override { return int(data[0]); }
    ~PolyLarge() override { ++poly_large_dtors; }
};

// the leaves must occupy distinct size classes for the "correct class on free" checks to have teeth
static_assert(cc::node_class_index_for<PolySmall>() != cc::node_class_index_for<PolyLarge>());

// User traits: recover the class index of the *actual* object, then destroy it. Order matters — the
// class index is read through the vtable, which is gone once the destructor has run.
struct PolyTraits
{
    static cc::node_class_index destroy_and_get_class_index(PolyBase& b)
    {
        auto const idx = b.which_class();
        b.~PolyBase(); // virtual -> runs the real leaf destructor
        return idx;
    }
};

template <class Derived, class... Args>
cc::poly_node_allocation<PolyBase, PolyTraits> make_poly(cc::node_allocator& alloc, Args&&... args)
{
    auto* const p = alloc.allocate_node_bytes(cc::node_class_index_for<Derived>(), sizeof(Derived), alignof(Derived));
    cc::poly_node_allocation<PolyBase, PolyTraits> r;
    r.ptr = new (cc::placement_new, p) Derived(cc::forward<Args>(args)...);
    return r;
}
} // namespace

TEST("poly_node_allocation - default constructed is invalid")
{
    cc::poly_node_allocation<PolyBase, PolyTraits> p;
    CHECK(!p.is_valid());
    CHECK(!p);
    CHECK(p.ptr == nullptr);
}

TEST("poly_node_allocation - smart pointer interface dispatches polymorphically")
{
    auto& alloc = cc::default_node_allocator();

    auto small = make_poly<PolySmall>(alloc, 42);
    CHECK(small.is_valid());
    CHECK(static_cast<bool>(small));
    CHECK((*small).value() == 42);
    CHECK(small->value() == 42);
    CHECK(small->which_class() == cc::node_class_index_for<PolySmall>());

    auto large = make_poly<PolyLarge>(alloc, 7);
    CHECK(large->value() == 7);
    CHECK(large->which_class() == cc::node_class_index_for<PolyLarge>());
}

TEST("poly_node_allocation - destructor runs the correct leaf destructor")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("small leaf")
    {
        reset_poly_counters();
        {
            auto p = make_poly<PolySmall>(alloc, 1);
            CHECK(poly_small_dtors == 0);
        }
        CHECK(poly_small_dtors == 1);
        CHECK(poly_large_dtors == 0);
    }

    SECTION("large leaf")
    {
        reset_poly_counters();
        {
            auto p = make_poly<PolyLarge>(alloc, 1);
            CHECK(poly_large_dtors == 0);
        }
        CHECK(poly_large_dtors == 1);
        CHECK(poly_small_dtors == 0);
    }
}

TEST("poly_node_allocation - reset destroys and empties, safe on empty")
{
    auto& alloc = cc::default_node_allocator();

    reset_poly_counters();
    auto p = make_poly<PolySmall>(alloc, 3);

    p.reset();
    CHECK(poly_small_dtors == 1);
    CHECK(!p.is_valid());
    CHECK(p.ptr == nullptr);

    p.reset(); // already empty
    CHECK(poly_small_dtors == 1);
}

TEST("poly_node_allocation - move construction transfers ownership")
{
    auto& alloc = cc::default_node_allocator();

    reset_poly_counters();
    auto p = make_poly<PolyLarge>(alloc, 55);
    PolyBase* const original_ptr = p.ptr;

    auto q = cc::move(p);

    CHECK(!p.is_valid());
    CHECK(q.is_valid());
    CHECK(q.ptr == original_ptr);
    CHECK(q->value() == 55);
    CHECK(poly_large_dtors == 0); // move must not destroy
}

TEST("poly_node_allocation - move assignment frees the previous object")
{
    auto& alloc = cc::default_node_allocator();

    reset_poly_counters();
    auto p = make_poly<PolySmall>(alloc, 1);
    auto q = make_poly<PolyLarge>(alloc, 2);

    q = cc::move(p);

    // q's previous PolyLarge is destroyed; the PolySmall is adopted and still readable
    CHECK(poly_large_dtors == 1);
    CHECK(poly_small_dtors == 0);
    CHECK(!p.is_valid());
    CHECK(q.is_valid());
    CHECK(q->value() == 1);
    CHECK(q->which_class() == cc::node_class_index_for<PolySmall>());
}

TEST("poly_node_allocation - mixed-leaf churn frees each in its own class")
{
    auto& alloc = cc::default_node_allocator();

    // Alternate leaf types per slot and churn. Freeing with the wrong class (e.g. treating a PolyLarge
    // as a PolySmall) would compute the wrong slab and corrupt the bitmap / trip the double-free assert.
    reset_poly_counters();
    std::array<cc::poly_node_allocation<PolyBase, PolyTraits>, 24> slots;

    for (int iter = 0; iter < 2400; ++iter)
    {
        int const idx = iter % 24;
        if (idx % 2 == 0)
        {
            slots[idx] = make_poly<PolySmall>(alloc, iter);
            CHECK(slots[idx]->value() == iter);
            CHECK(slots[idx]->which_class() == cc::node_class_index_for<PolySmall>());
        }
        else
        {
            slots[idx] = make_poly<PolyLarge>(alloc, iter);
            CHECK(slots[idx]->value() == iter);
            CHECK(slots[idx]->which_class() == cc::node_class_index_for<PolyLarge>());
        }
    }

    // every reallocation of a live slot destroyed its predecessor -> plenty of both leaf dtors ran
    CHECK(poly_small_dtors > 0);
    CHECK(poly_large_dtors > 0);
}
