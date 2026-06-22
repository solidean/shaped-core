#include <clean-core/allocation.hh>
#include <clean-core/span.hh>
#include <clean-core/utility.hh>

#include <nexus/test.hh>

#include <vector>

namespace
{
// Instrumented type that tracks construction and destruction
struct Tracked
{
    int value = 0;
    static inline int default_ctor_count = 0;
    static inline int copy_ctor_count = 0;
    static inline int move_ctor_count = 0;
    static inline int dtor_count = 0;
    static inline std::vector<int>* destruction_order = nullptr;

    static void reset_counters()
    {
        default_ctor_count = 0;
        copy_ctor_count = 0;
        move_ctor_count = 0;
        dtor_count = 0;
        destruction_order = nullptr;
    }

    Tracked() { ++default_ctor_count; }

    explicit Tracked(int v) : value(v) { ++default_ctor_count; }

    Tracked(Tracked const& rhs) : value(rhs.value) { ++copy_ctor_count; }

    Tracked(Tracked&& rhs) noexcept : value(rhs.value) { ++move_ctor_count; }

    Tracked& operator=(Tracked const& rhs)
    {
        value = rhs.value;
        return *this;
    }

    Tracked& operator=(Tracked&& rhs) noexcept
    {
        value = rhs.value;
        return *this;
    }

    ~Tracked()
    {
        ++dtor_count;
        if (destruction_order)
            destruction_order->push_back(value);
    }
};
} // namespace

TEST("allocation - default construction")
{
    cc::allocation<int> alloc;

    CHECK(alloc.obj_start == nullptr);
    CHECK(alloc.obj_end == nullptr);
    CHECK(alloc.alloc_start == nullptr);
    CHECK(alloc.alloc_end == nullptr);
    CHECK(alloc.alignment == 0);
    CHECK(alloc.custom_resource == nullptr);
    CHECK(!alloc.is_valid());
    CHECK(alloc.obj_span().size() == 0);
    CHECK(alloc.alloc_size_bytes() == 0);
}

TEST("allocation - create_empty")
{
    auto alloc = cc::allocation<int>::create_empty(10, alignof(int), nullptr);

    CHECK(alloc.is_valid());
    CHECK(alloc.alloc_start != nullptr);
    CHECK(alloc.alloc_end > alloc.alloc_start);
    CHECK(alloc.alloc_size_bytes() >= 10 * sizeof(int));
    CHECK(alloc.obj_start == (int*)alloc.alloc_start);
    CHECK(alloc.obj_end == alloc.obj_start); // no live objects
    CHECK(alloc.obj_span().size() == 0);
    CHECK(alloc.custom_resource == nullptr);
    CHECK(alloc.alignment == alignof(int));
}

TEST("allocation - create_empty with zero size")
{
    auto alloc = cc::allocation<int>::create_empty(0, alignof(int), nullptr);

    CHECK(!alloc.is_valid());
    CHECK(alloc.alloc_start == nullptr);
    CHECK(alloc.alloc_end == nullptr);
    CHECK(alloc.obj_start == nullptr);
    CHECK(alloc.obj_end == nullptr);
    CHECK(alloc.alloc_size_bytes() == 0);
    CHECK(alloc.obj_span().size() == 0);
}

TEST("allocation - create_defaulted")
{
    Tracked::reset_counters();

    {
        auto alloc = cc::allocation<Tracked>::create_defaulted(5, nullptr);

        CHECK(alloc.is_valid());
        CHECK(alloc.obj_span().size() == 5);
        CHECK(alloc.alloc_size_bytes() >= 5 * sizeof(Tracked));
        CHECK(alloc.obj_start == (Tracked*)alloc.alloc_start);
        CHECK(alloc.obj_end == alloc.obj_start + 5);
        CHECK(Tracked::default_ctor_count == 5);

        // Check all objects were default-constructed (value == 0)
        for (auto const& obj : alloc.obj_span())
            CHECK(obj.value == 0);
    }

    // All objects should be destroyed
    CHECK(Tracked::dtor_count == 5);
}

TEST("allocation - create_filled")
{
    Tracked::reset_counters();

    {
        Tracked fill_value(42);
        Tracked::reset_counters(); // reset after creating fill_value

        auto alloc = cc::allocation<Tracked>::create_filled(7, fill_value, nullptr);

        CHECK(alloc.is_valid());
        CHECK(alloc.obj_span().size() == 7);
        CHECK(Tracked::copy_ctor_count == 7);

        // Check all objects have the fill value
        for (auto const& obj : alloc.obj_span())
            CHECK(obj.value == 42);
    }

    // fill value is destroyed as well
    CHECK(Tracked::dtor_count == 7 + 1);
}

TEST("allocation - create_uninitialized")
{
    auto alloc = cc::allocation<int>::create_uninitialized(8, nullptr);

    CHECK(alloc.is_valid());
    CHECK(alloc.obj_span().size() == 8);
    CHECK(alloc.alloc_size_bytes() >= 8 * sizeof(int));

    // obj_end should be set to the end (uninitialized but marked as live)
    CHECK(alloc.obj_end == alloc.obj_start + 8);

    // Initialize the memory manually
    for (int i = 0; i < 8; ++i)
        alloc.obj_start[i] = i * 10;

    // Verify we can read back what we wrote
    for (int i = 0; i < 8; ++i)
        CHECK(alloc.obj_start[i] == i * 10);
}

TEST("allocation - create_copy_of span")
{
    Tracked::reset_counters();

    {
        // Create source data
        Tracked source[3] = {Tracked(10), Tracked(20), Tracked(30)};
        Tracked::reset_counters(); // reset after creating source

        cc::span<Tracked const> source_span(source, 3);
        auto alloc = cc::allocation<Tracked>::create_copy_of(source_span, nullptr);

        CHECK(alloc.is_valid());
        CHECK(alloc.obj_span().size() == 3);
        CHECK(Tracked::copy_ctor_count == 3);

        // Verify copied values
        CHECK(alloc.obj_start[0].value == 10);
        CHECK(alloc.obj_start[1].value == 20);
        CHECK(alloc.obj_start[2].value == 30);
    }

    // sources are destroyed as well
    CHECK(Tracked::dtor_count == 3 + 3);
}

TEST("allocation - create_copy_of allocation")
{
    Tracked::reset_counters();

    {
        auto src = cc::allocation<Tracked>::create_filled(4, Tracked(99), nullptr);
        Tracked::reset_counters(); // reset after creating source

        auto copy = cc::allocation<Tracked>::create_copy_of(src, nullptr);

        CHECK(copy.is_valid());
        CHECK(copy.obj_span().size() == 4);
        CHECK(Tracked::copy_ctor_count == 4);

        // Verify copied values
        for (auto const& obj : copy.obj_span())
            CHECK(obj.value == 99);

        // Verify independent allocation
        CHECK(copy.alloc_start != src.alloc_start);
    }
}

TEST("allocation - move construction")
{
    Tracked::reset_counters();

    auto src = cc::allocation<Tracked>::create_filled(3, Tracked(42), nullptr);
    auto const* original_start = src.alloc_start;
    auto const* original_obj_start = src.obj_start;

    Tracked::reset_counters(); // reset after creation

    auto dst = cc::move(src);

    // Verify dst took ownership
    CHECK(dst.alloc_start == original_start);
    CHECK(dst.obj_start == original_obj_start);
    CHECK(dst.obj_span().size() == 3);

    // Verify src is cleared
    CHECK(src.alloc_start == nullptr);
    CHECK(src.obj_start == nullptr);
    CHECK(src.obj_end == nullptr);
    CHECK(!src.is_valid());

    // No objects should be copied or destroyed during move
    CHECK(Tracked::copy_ctor_count == 0);
    CHECK(Tracked::move_ctor_count == 0);
    CHECK(Tracked::dtor_count == 0);
}

TEST("allocation - move assignment")
{
    Tracked::reset_counters();

    auto dst = cc::allocation<Tracked>::create_filled(2, Tracked(10), nullptr);
    auto src = cc::allocation<Tracked>::create_filled(3, Tracked(42), nullptr);

    auto const* src_original_start = src.alloc_start;

    Tracked::reset_counters(); // reset after creation

    dst = cc::move(src);

    // dst should now own src's allocation
    CHECK(dst.alloc_start == src_original_start);
    CHECK(dst.obj_span().size() == 3);
    for (auto const& obj : dst.obj_span())
        CHECK(obj.value == 42);

    // src should be cleared
    CHECK(src.alloc_start == nullptr);
    CHECK(!src.is_valid());

    // The old dst objects (2 of them) should be destroyed
    CHECK(Tracked::dtor_count == 2);
}

TEST("allocation - destruction order")
{
    std::vector<int> order;
    Tracked::reset_counters();
    Tracked::destruction_order = &order;

    {
        auto alloc = cc::allocation<Tracked>::create_defaulted(5, nullptr);

        // Set distinct values
        for (int i = 0; i < 5; ++i)
            alloc.obj_start[i].value = i;
    }

    // Objects should be destroyed in reverse order
    REQUIRE(order.size() == 5);
    CHECK(order[0] == 4);
    CHECK(order[1] == 3);
    CHECK(order[2] == 2);
    CHECK(order[3] == 1);
    CHECK(order[4] == 0);

    Tracked::destruction_order = nullptr;
}

TEST("allocation - resource resolution")
{
    auto alloc = cc::allocation<int>::create_empty(10, alignof(int), nullptr);

    // custom_resource is nullptr, so resource() should return default_memory_resource
    CHECK(alloc.custom_resource == nullptr);
    CHECK(&alloc.resource() == cc::default_memory_resource);
}

TEST("allocation - obj_span provides correct view")
{
    auto alloc = cc::allocation<int>::create_defaulted(6, nullptr);

    // Modify objects
    for (int i = 0; i < 6; ++i)
        alloc.obj_start[i] = i * 100;

    auto span = alloc.obj_span();

    CHECK(span.size() == 6);
    CHECK(span.data() == alloc.obj_start);

    // Verify span content
    for (int i = 0; i < 6; ++i)
        CHECK(span[i] == i * 100);
}

TEST("allocation - alloc_size_bytes")
{
    auto alloc = cc::allocation<int>::create_empty(15, alignof(int), nullptr);

    // alloc_size_bytes should be at least the requested byte count
    CHECK(alloc.alloc_size_bytes() >= 15 * sizeof(int));
    CHECK(alloc.alloc_size_bytes() == alloc.alloc_end - alloc.alloc_start);
}

TEST("allocation - is_valid semantic")
{
    cc::allocation<int> empty_default;
    CHECK(!empty_default.is_valid());

    auto empty_zero = cc::allocation<int>::create_empty(0, alignof(int), nullptr);
    CHECK(!empty_zero.is_valid());

    auto valid = cc::allocation<int>::create_empty(1, alignof(int), nullptr);
    CHECK(valid.is_valid());
}

TEST("allocation - alignment stored correctly")
{
    auto alloc = cc::allocation<int>::create_empty(10, 64, nullptr);

    CHECK(alloc.alignment == 64);
    CHECK(cc::is_aligned(alloc.alloc_start, 64));
}
