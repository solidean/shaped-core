#include <clean-core/array.hh>
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

struct TrackedCopy
{
    int value = 0;
    static inline int copy_ctor_count = 0;

    static void reset_counters() { copy_ctor_count = 0; }

    TrackedCopy() = default;
    explicit TrackedCopy(int v) : value(v) {}

    TrackedCopy(TrackedCopy const& rhs) : value(rhs.value) { ++copy_ctor_count; }

    TrackedCopy& operator=(TrackedCopy const& rhs)
    {
        value = rhs.value;
        return *this;
    }
};

struct TrackedMove
{
    int value = 0;
    static inline int move_ctor_count = 0;

    static void reset_counters() { move_ctor_count = 0; }

    TrackedMove() = default;
    explicit TrackedMove(int v) : value(v) {}

    TrackedMove(TrackedMove const&) = delete;
    TrackedMove& operator=(TrackedMove const&) = delete;

    TrackedMove(TrackedMove&& rhs) noexcept : value(rhs.value)
    {
        ++move_ctor_count;
        rhs.value = -1;
    }

    TrackedMove& operator=(TrackedMove&& rhs) noexcept
    {
        value = rhs.value;
        rhs.value = -1;
        return *this;
    }
};

struct MoveOnly
{
    MoveOnly() = default;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    MoveOnly(MoveOnly const&) = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;
};

// Counting memory resource
struct CountingResource : cc::memory_resource
{
    int allocations = 0;
    int deallocations = 0;
    cc::isize total_allocated_bytes = 0;
    cc::isize total_deallocated_bytes = 0;

    CountingResource()
    {
        allocate_bytes = [](cc::byte** out_ptr, cc::isize min_bytes, cc::isize max_bytes, cc::isize alignment,
                            void* userdata) -> cc::isize
        {
            auto* self = static_cast<CountingResource*>(userdata);
            ++self->allocations;
            if (min_bytes == 0)
            {
                *out_ptr = nullptr;
                return 0;
            }
            // Allocate exactly min_bytes (not using max_bytes for size class rounding)
            *out_ptr = static_cast<cc::byte*>(::operator new(min_bytes, std::align_val_t(alignment)));
            self->total_allocated_bytes += min_bytes;
            return min_bytes;
        };

        deallocate_bytes = [](cc::byte* p, cc::isize bytes, cc::isize alignment, void* userdata)
        {
            auto* self = static_cast<CountingResource*>(userdata);
            ++self->deallocations;
            self->total_deallocated_bytes += bytes;
            if (p != nullptr)
                ::operator delete(p, std::align_val_t(alignment));
        };

        userdata = this;
    }

    void reset()
    {
        allocations = 0;
        deallocations = 0;
        total_allocated_bytes = 0;
        total_deallocated_bytes = 0;
    }
};
} // namespace

TEST("array - default construction invariants")
{
    SECTION("empty state - int")
    {
        cc::array<int> a;
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);
        CHECK(a.begin() == a.end());
    }

    SECTION("empty state - Tracked")
    {
        Tracked::reset_counters();
        {
            cc::array<Tracked> a;
            CHECK(a.size() == 0);
            CHECK(a.empty() == true);
            CHECK(a.begin() == a.end());
        }
        CHECK(Tracked::default_ctor_count == 0);
        CHECK(Tracked::dtor_count == 0);
    }

    SECTION("iteration on empty array")
    {
        cc::array<int> a;
        int count = 0;
        for ([[maybe_unused]] auto const& val : a)
            ++count;
        CHECK(count == 0);
    }
}

TEST("array - create_defaulted")
{
    SECTION("int - size 0")
    {
        auto a = cc::array<int>::create_defaulted(0);
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);
    }

    SECTION("int - size 1")
    {
        auto a = cc::array<int>::create_defaulted(1);
        CHECK(a.size() == 1);
        CHECK(a.empty() == false);
        CHECK(a.front() == 0);
        CHECK(a.back() == 0);
        CHECK(a[0] == 0);
    }

    SECTION("int - size 5")
    {
        auto a = cc::array<int>::create_defaulted(5);
        CHECK(a.size() == 5);
        CHECK(a.empty() == false);
        CHECK(a.front() == 0);
        CHECK(a.back() == 0);
        for (int i = 0; i < 5; ++i)
            CHECK(a[i] == 0);
    }

    SECTION("int - size 64")
    {
        auto a = cc::array<int>::create_defaulted(64);
        CHECK(a.size() == 64);
        CHECK(a.empty() == false);
    }

    SECTION("Tracked - size 0")
    {
        Tracked::reset_counters();
        {
            auto a = cc::array<Tracked>::create_defaulted(0);
            CHECK(a.size() == 0);
            CHECK(a.empty() == true);
        }
        CHECK(Tracked::default_ctor_count == 0);
        CHECK(Tracked::dtor_count == 0);
    }

    SECTION("Tracked - size 1")
    {
        Tracked::reset_counters();
        {
            auto a = cc::array<Tracked>::create_defaulted(1);
            CHECK(a.size() == 1);
            CHECK(a.empty() == false);
            CHECK(a.front().value == 0);
            CHECK(a.back().value == 0);
        }
        CHECK(Tracked::default_ctor_count == 1);
        CHECK(Tracked::dtor_count == 1);
    }

    SECTION("Tracked - size 5")
    {
        Tracked::reset_counters();
        {
            auto a = cc::array<Tracked>::create_defaulted(5);
            CHECK(a.size() == 5);
        }
        CHECK(Tracked::default_ctor_count == 5);
        CHECK(Tracked::dtor_count == 5);
    }

    SECTION("Tracked - size 64")
    {
        Tracked::reset_counters();
        {
            auto a = cc::array<Tracked>::create_defaulted(64);
            CHECK(a.size() == 64);
        }
        CHECK(Tracked::default_ctor_count == 64);
        CHECK(Tracked::dtor_count == 64);
    }
}

TEST("array - create_filled")
{
    SECTION("int - size 0")
    {
        auto a = cc::array<int>::create_filled(0, 7);
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);
    }

    SECTION("int - size 1")
    {
        auto a = cc::array<int>::create_filled(1, 7);
        CHECK(a.size() == 1);
        CHECK(a[0] == 7);
    }

    SECTION("int - size 10")
    {
        auto a = cc::array<int>::create_filled(10, 7);
        CHECK(a.size() == 10);
        for (int i = 0; i < 10; ++i)
            CHECK(a[i] == 7);
    }

    SECTION("TrackedCopy - size 0")
    {
        TrackedCopy::reset_counters();
        {
            auto a = cc::array<TrackedCopy>::create_filled(0, TrackedCopy(42));
        }
        CHECK(TrackedCopy::copy_ctor_count == 0);
    }

    SECTION("TrackedCopy - size 1")
    {
        TrackedCopy::reset_counters();
        {
            auto const value = TrackedCopy(42);
            TrackedCopy::reset_counters();
            auto a = cc::array<TrackedCopy>::create_filled(1, value);
            CHECK(a.size() == 1);
            CHECK(a[0].value == 42);
        }
        CHECK(TrackedCopy::copy_ctor_count == 1);
    }

    SECTION("TrackedCopy - size 10")
    {
        TrackedCopy::reset_counters();
        {
            auto const value = TrackedCopy(99);
            TrackedCopy::reset_counters();
            auto a = cc::array<TrackedCopy>::create_filled(10, value);
            CHECK(a.size() == 10);
            for (int i = 0; i < 10; ++i)
                CHECK(a[i].value == 99);
        }
        CHECK(TrackedCopy::copy_ctor_count == 10);
    }
}

TEST("array - create_uninitialized")
{
    SECTION("uint32_t - size 0")
    {
        auto a = cc::array<uint32_t>::create_uninitialized(0);
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);
    }

    SECTION("uint32_t - size 1")
    {
        auto a = cc::array<uint32_t>::create_uninitialized(1);
        CHECK(a.size() == 1);
        a[0] = 0xDEADBEEF;
        CHECK(a[0] == 0xDEADBEEF);
    }

    SECTION("uint32_t - size 128")
    {
        auto a = cc::array<uint32_t>::create_uninitialized(128);
        CHECK(a.size() == 128);
        for (int i = 0; i < 128; ++i)
        {
            a[i] = static_cast<uint32_t>(i * 1000);
        }
        for (int i = 0; i < 128; ++i)
        {
            CHECK(a[i] == static_cast<uint32_t>(i * 1000));
        }
    }

    SECTION("uint32_t - data() access")
    {
        auto a = cc::array<uint32_t>::create_uninitialized(10);
        for (int i = 0; i < 10; ++i)
            a.data()[i] = static_cast<uint32_t>(i);
        for (int i = 0; i < 10; ++i)
            CHECK(a.data()[i] == static_cast<uint32_t>(i));
    }
}

TEST("array - create_copy_of")
{
    SECTION("int - size 0")
    {
        int const source[] = {0};
        auto a = cc::array<int>::create_copy_of(cc::span<int const>{});
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);
    }

    SECTION("int - size 3")
    {
        int const source[] = {1, 2, 3};
        auto a = cc::array<int>::create_copy_of(cc::span<int const>(source));
        CHECK(a.size() == 3);
        CHECK(a[0] == 1);
        CHECK(a[1] == 2);
        CHECK(a[2] == 3);
        CHECK(a.data() != source);

        // Verify no aliasing (mutate through different array)
        auto b = cc::array<int>::create_copy_of(cc::span<int const>(source));
        b[1] = 99;
        CHECK(a[1] == 2);
    }

    SECTION("int - size 31")
    {
        int source[31];
        for (int i = 0; i < 31; ++i)
            source[i] = i * 10;

        auto a = cc::array<int>::create_copy_of(cc::span<int const>(source));
        CHECK(a.size() == 31);
        CHECK(a.data() != source);

        for (int i = 0; i < 31; ++i)
            CHECK(a[i] == i * 10);
    }

    SECTION("TrackedCopy - size 0")
    {
        TrackedCopy::reset_counters();
        {
            auto a = cc::array<TrackedCopy>::create_copy_of(cc::span<TrackedCopy const>{});
            CHECK(a.size() == 0);
        }
        CHECK(TrackedCopy::copy_ctor_count == 0);
    }

    SECTION("TrackedCopy - size 3")
    {
        TrackedCopy const source[3] = {TrackedCopy(10), TrackedCopy(20), TrackedCopy(30)};
        TrackedCopy::reset_counters();

        auto a = cc::array<TrackedCopy>::create_copy_of(cc::span<TrackedCopy const>(source));
        CHECK(a.size() == 3);
        CHECK(a[0].value == 10);
        CHECK(a[1].value == 20);
        CHECK(a[2].value == 30);
        CHECK(TrackedCopy::copy_ctor_count == 3);

        // Verify no aliasing
        CHECK(a.data() != source);
    }

    SECTION("TrackedCopy - size 31")
    {
        TrackedCopy source[31];
        for (int i = 0; i < 31; ++i)
            source[i] = TrackedCopy(i);
        TrackedCopy::reset_counters();

        auto a = cc::array<TrackedCopy>::create_copy_of(cc::span<TrackedCopy const>(source));
        CHECK(a.size() == 31);
        CHECK(TrackedCopy::copy_ctor_count == 31);
        CHECK(a.data() != source);
    }
}

TEST("array - copy constructor deep copy")
{
    SECTION("int - basic deep copy")
    {
        auto a = cc::array<int>::create_filled(5, 42);
        auto b(a);

        CHECK(b.size() == 5);
        CHECK(b.data() != a.data());
        for (int i = 0; i < 5; ++i)
            CHECK(b[i] == 42);

        // Verify independence
        a[0] = 99;
        CHECK(b[0] == 42);
    }

    SECTION("TrackedCopy - deep copy with unique objects")
    {
        auto a = cc::array<TrackedCopy>::create_defaulted(3);
        a[0].value = 10;
        a[1].value = 20;
        a[2].value = 30;

        TrackedCopy::reset_counters();
        auto b(a);

        CHECK(b.size() == 3);
        CHECK(b[0].value == 10);
        CHECK(b[1].value == 20);
        CHECK(b[2].value == 30);
        CHECK(b.data() != a.data());
        CHECK(TrackedCopy::copy_ctor_count == 3);
    }

    SECTION("TrackedCopy - destroying copy doesn't affect original")
    {
        auto a = cc::array<TrackedCopy>::create_defaulted(5);
        for (int i = 0; i < 5; ++i)
            a[i].value = i;

        {
            auto b(a);
            CHECK(b.size() == 5);
        }

        // a should still be intact
        CHECK(a.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(a[i].value == i);
    }
}

TEST("array - copy assignment keeps LHS resource")
{
    SECTION("different sizes and resources")
    {
        CountingResource resA;
        CountingResource resB;

        auto lhs = cc::array<int>::create_defaulted(3, &resA);
        auto rhs = cc::array<int>::create_defaulted(5, &resB);
        rhs[0] = 10;
        rhs[1] = 20;
        rhs[2] = 30;
        rhs[3] = 40;
        rhs[4] = 50;

        resA.reset();
        resB.reset();

        lhs = rhs;

        CHECK(lhs.size() == 5);
        CHECK(lhs[0] == 10);
        CHECK(lhs[4] == 50);

        // LHS should have allocated with resource A
        CHECK(resA.allocations == 1);
        CHECK(resA.deallocations == 1); // old allocation freed

        // RHS resource B should be untouched
        CHECK(resB.allocations == 0);
        CHECK(resB.deallocations == 0);
    }

    SECTION("self-assign does nothing")
    {
        CountingResource res;
        auto a = cc::array<int>::create_defaulted(5, &res);
        res.reset();

        a = a;

        CHECK(a.size() == 5);
        CHECK(res.allocations == 0);
        CHECK(res.deallocations == 0);
    }

    SECTION("Tracked - values equal after assign")
    {
        CountingResource resA;
        CountingResource resB;

        auto lhs = cc::array<Tracked>::create_defaulted(2, &resA);
        auto rhs = cc::array<Tracked>::create_defaulted(3, &resB);
        rhs[0].value = 100;
        rhs[1].value = 200;
        rhs[2].value = 300;

        lhs = rhs;

        CHECK(lhs.size() == 3);
        CHECK(lhs[0].value == 100);
        CHECK(lhs[1].value == 200);
        CHECK(lhs[2].value == 300);

        // Verify rhs unchanged
        CHECK(rhs.size() == 3);
        CHECK(rhs[0].value == 100);
    }
}

TEST("array - move constructor transfers ownership")
{
    SECTION("int - size 0")
    {
        CountingResource res;
        auto a = cc::array<int>::create_defaulted(0, &res);
        res.reset();

        auto b = cc::move(a);

        CHECK(b.size() == 0);
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);

        // No deallocations during move
        CHECK(res.deallocations == 0);
    }

    SECTION("int - size 1")
    {
        CountingResource res;
        auto a = cc::array<int>::create_defaulted(1, &res);
        a[0] = 42;
        auto const old_size = a.size();
        res.reset();

        auto b = cc::move(a);

        CHECK(b.size() == old_size);
        CHECK(b[0] == 42);
        CHECK(a.size() == 0);
        CHECK(a.empty() == true);

        // No deallocations during move
        CHECK(res.deallocations == 0);
    }

    SECTION("int - size 9")
    {
        CountingResource res;
        auto a = cc::array<int>::create_defaulted(9, &res);
        for (int i = 0; i < 9; ++i)
            a[i] = i * 10;
        auto const old_size = a.size();
        res.reset();

        auto b = cc::move(a);

        CHECK(b.size() == old_size);
        for (int i = 0; i < 9; ++i)
            CHECK(b[i] == i * 10);
        CHECK(a.size() == 0);

        // No deallocations during move
        CHECK(res.deallocations == 0);
    }

    SECTION("TrackedMove - single deallocation at end")
    {
        CountingResource res;
        TrackedMove::reset_counters();

        {
            auto a = cc::array<TrackedMove>::create_defaulted(5, &res);
            res.reset();

            auto b = cc::move(a);
            CHECK(b.size() == 5);
            CHECK(a.size() == 0);

            // No deallocations yet
            CHECK(res.deallocations == 0);
        }

        // Exactly one deallocation at end
        CHECK(res.deallocations == 1);
    }
}

TEST("array - move assignment transfers ownership")
{
    SECTION("different sizes and resources")
    {
        CountingResource resA;
        CountingResource resB;

        auto a = cc::array<int>::create_defaulted(3, &resA);
        a[0] = 1;
        a[1] = 2;
        a[2] = 3;

        auto b = cc::array<int>::create_defaulted(5, &resB);
        b[0] = 10;
        b[1] = 20;
        b[2] = 30;
        b[3] = 40;
        b[4] = 50;

        resA.reset();
        resB.reset();

        a = cc::move(b);

        // a now has b's data
        CHECK(a.size() == 5);
        CHECK(a[0] == 10);
        CHECK(a[4] == 50);

        // b is now empty
        CHECK(b.size() == 0);
        CHECK(b.empty() == true);

        // Old a's allocation was freed via resA
        CHECK(resA.deallocations == 1);

        // resB had no deallocations (data transferred to a)
        CHECK(resB.deallocations == 0);
    }

    SECTION("Tracked - proper element destruction")
    {
        CountingResource resA;
        CountingResource resB;

        Tracked::reset_counters();
        {
            auto a = cc::array<Tracked>::create_defaulted(3, &resA);
            auto b = cc::array<Tracked>::create_defaulted(5, &resB);

            auto const initial_ctor_count = Tracked::default_ctor_count;
            auto const initial_dtor_count = Tracked::dtor_count;

            a = cc::move(b);

            // Old a elements destroyed
            CHECK(Tracked::dtor_count == initial_dtor_count + 3);

            CHECK(a.size() == 5);
            CHECK(b.size() == 0);
        }

        // All elements destroyed at end
        CHECK(Tracked::default_ctor_count == Tracked::dtor_count);
    }

    SECTION("total allocations and deallocations match")
    {
        CountingResource resA;
        CountingResource resB;

        {
            auto a = cc::array<int>::create_defaulted(10, &resA);
            auto b = cc::array<int>::create_defaulted(20, &resB);

            auto const total_allocs_before = resA.allocations + resB.allocations;

            a = cc::move(b);

            // One deallocation from old a
            CHECK(resA.deallocations == 1);
        }

        // At end, all allocations should be deallocated
        CHECK(resA.allocations == resA.deallocations);
        CHECK(resB.allocations == resB.deallocations);
    }
}

TEST("array - move-only element type")
{
    SECTION("create_defaulted compiles")
    {
        auto a = cc::array<MoveOnly>::create_defaulted(5);
        CHECK(a.size() == 5);
    }

    SECTION("create_filled does not compile")
    {
        // This should fail to compile if uncommented
        // MoveOnly value;
        // auto a = cc::array<MoveOnly>::create_filled(5, value);

        // Verify at compile time that create_filled is not available
        static_assert(!std::is_copy_constructible_v<MoveOnly>, "MoveOnly should not be copy constructible");

        SUCCEED(); // just static checks
    }

    SECTION("copy constructor does not compile")
    {
        // This should fail to compile if uncommented
        // auto a = cc::array<MoveOnly>::create_defaulted(5);
        // auto b = a;

        // array should be copy constructible iff T is copy constructible
        static_assert(std::is_copy_constructible_v<cc::array<int>>, "array<int> should be copy constructible");
        static_assert(!std::is_copy_constructible_v<MoveOnly>, "MoveOnly should not be copy constructible");

        SUCCEED(); // just static checks
    }

    SECTION("copy assignment does not compile")
    {
        // array should be copy assignable iff T is copy assignable
        static_assert(std::is_copy_assignable_v<cc::array<int>>, "array<int> should be copy assignable");
        static_assert(!std::is_copy_assignable_v<MoveOnly>, "MoveOnly should not be copy assignable");

        SUCCEED(); // just static checks
    }

    SECTION("move constructor compiles")
    {
        auto a = cc::array<MoveOnly>::create_defaulted(5);
        auto b = cc::move(a);
        CHECK(b.size() == 5);
        CHECK(a.size() == 0);
    }

    SECTION("move assignment compiles")
    {
        auto a = cc::array<MoveOnly>::create_defaulted(5);
        auto b = cc::array<MoveOnly>::create_defaulted(3);
        b = cc::move(a);
        CHECK(b.size() == 5);
        CHECK(a.size() == 0);
    }
}

TEST("array - iterators and data contiguity")
{
    SECTION("int - size 10")
    {
        auto a = cc::array<int>::create_defaulted(10);
        CHECK(a.end() - a.begin() == 10);
        CHECK(a.end() - a.begin() == a.size());

        for (int i = 0; i < 10; ++i)
            CHECK(&a[i] == a.data() + i);
    }

    SECTION("mutation through iteration")
    {
        auto a = cc::array<int>::create_defaulted(10);
        int value = 0;
        for (auto& elem : a)
            elem = value++;

        CHECK(a[0] == 0);
        CHECK(a[5] == 5);
        CHECK(a[9] == 9);
    }

    SECTION("const array - const iterators")
    {
        auto const a = cc::array<int>::create_filled(5, 42);
        CHECK(a.end() - a.begin() == 5);

        int sum = 0;
        for (auto const& elem : a)
            sum += elem;
        CHECK(sum == 42 * 5);
    }
}

TEST("array - front/back/operator[] consistency")
{
    SECTION("int - size 1")
    {
        auto a = cc::array<int>::create_filled(1, 99);
        CHECK(a.front() == 99);
        CHECK(a.back() == 99);
        CHECK(a[0] == 99);
        CHECK(a.front() == a.back());
        CHECK(a.front() == a[0]);
    }

    SECTION("int - size > 1")
    {
        auto a = cc::array<int>::create_defaulted(10);
        a[0] = 100;
        a[9] = 200;

        CHECK(a.front() == 100);
        CHECK(a.back() == 200);
        CHECK(a.front() == a[0]);
        CHECK(a.back() == a[9]);
    }

    SECTION("Tracked - size 1")
    {
        auto a = cc::array<Tracked>::create_defaulted(1);
        a[0].value = 42;

        CHECK(a.front().value == 42);
        CHECK(a.back().value == 42);
        CHECK(a[0].value == 42);
    }

    SECTION("Tracked - size > 1")
    {
        auto a = cc::array<Tracked>::create_defaulted(5);
        a[0].value = 111;
        a[4].value = 222;

        CHECK(a.front().value == 111);
        CHECK(a.back().value == 222);
        CHECK(a.front().value == a[0].value);
        CHECK(a.back().value == a[4].value);
    }
}

TEST("array - create_from_allocation adopts without copying")
{
    SECTION("int - adopts allocation")
    {
        auto alloc = cc::allocation<int>::create_filled(5, 42, nullptr);
        auto const original_data = alloc.obj_start;
        auto const original_size = alloc.obj_end - alloc.obj_start;

        auto a = cc::array<int>::create_from_allocation(cc::move(alloc));

        CHECK(a.data() == original_data);
        CHECK(a.size() == original_size);
        CHECK(a[0] == 42);
        CHECK(a[4] == 42);
    }

    SECTION("no extra allocations")
    {
        CountingResource res;
        {
            auto alloc = cc::allocation<int>::create_defaulted(10, &res);
            res.reset();

            auto a = cc::array<int>::create_from_allocation(cc::move(alloc));
            CHECK(a.size() == 10);

            // No new allocations
            CHECK(res.allocations == 0);
        }

        // Exactly one deallocation at end
        CHECK(res.deallocations == 1);
    }

    SECTION("Tracked - no copy construction")
    {
        Tracked::reset_counters();
        {
            auto alloc = cc::allocation<Tracked>::create_defaulted(7, nullptr);
            auto const ctor_count = Tracked::default_ctor_count;

            auto a = cc::array<Tracked>::create_from_allocation(cc::move(alloc));
            CHECK(a.size() == 7);

            // No additional construction
            CHECK(Tracked::default_ctor_count == ctor_count);
            CHECK(Tracked::copy_ctor_count == 0);
        }
    }
}

TEST("array - extract_allocation round-trip")
{
    SECTION("array becomes empty but keeps resource")
    {
        CountingResource res;
        auto a = cc::array<int>::create_defaulted(10, &res);
        for (int i = 0; i < 10; ++i)
            a[i] = i * 5;

        auto alloc = a.extract_allocation();

        CHECK(a.size() == 0);
        CHECK(a.empty() == true);
        CHECK(alloc.obj_end - alloc.obj_start == 10);
        CHECK(alloc.custom_resource == &res);

        for (int i = 0; i < 10; ++i)
            CHECK(alloc.obj_start[i] == i * 5);
    }

    SECTION("round-trip preserves values")
    {
        CountingResource res;
        auto a = cc::array<int>::create_defaulted(5, &res);
        a[0] = 10;
        a[1] = 20;
        a[2] = 30;
        a[3] = 40;
        a[4] = 50;

        auto alloc = a.extract_allocation();
        CHECK(a.size() == 0);

        auto b = cc::array<int>::create_from_allocation(cc::move(alloc));
        CHECK(b.size() == 5);
        CHECK(b[0] == 10);
        CHECK(b[1] == 20);
        CHECK(b[2] == 30);
        CHECK(b[3] == 40);
        CHECK(b[4] == 50);
    }

    SECTION("exactly one deallocation on resource")
    {
        CountingResource res;
        {
            auto a = cc::array<int>::create_defaulted(10, &res);
            res.reset();

            auto alloc = a.extract_allocation();
            CHECK(a.size() == 0);

            auto b = cc::array<int>::create_from_allocation(cc::move(alloc));

            // No deallocations yet
            CHECK(res.deallocations == 0);
        }

        // Exactly one deallocation at end
        CHECK(res.deallocations == 1);
    }
}

TEST("array - destruction order")
{
    SECTION("reverse destruction order")
    {
        std::vector<int> destruction_sequence;
        Tracked::reset_counters();
        Tracked::destruction_order = &destruction_sequence;

        {
            auto a = cc::array<Tracked>::create_defaulted(5);
            a[0].value = 0;
            a[1].value = 1;
            a[2].value = 2;
            a[3].value = 3;
            a[4].value = 4;
        }

        CHECK(destruction_sequence.size() == 5);
        CHECK(destruction_sequence[0] == 4);
        CHECK(destruction_sequence[1] == 3);
        CHECK(destruction_sequence[2] == 2);
        CHECK(destruction_sequence[3] == 1);
        CHECK(destruction_sequence[4] == 0);

        Tracked::destruction_order = nullptr;
    }

    SECTION("create_filled - reverse destruction")
    {
        std::vector<int> destruction_sequence;
        Tracked::reset_counters();
        Tracked::destruction_order = &destruction_sequence;

        {
            auto a = cc::array<Tracked>::create_defaulted(3);
            a[0].value = 10;
            a[1].value = 20;
            a[2].value = 30;
        }

        CHECK(destruction_sequence.size() == 3);
        CHECK(destruction_sequence[0] == 30);
        CHECK(destruction_sequence[1] == 20);
        CHECK(destruction_sequence[2] == 10);

        Tracked::destruction_order = nullptr;
    }

    SECTION("create_copy_of - reverse destruction")
    {
        std::vector<int> destruction_sequence;
        Tracked::reset_counters();
        Tracked::destruction_order = &destruction_sequence;

        {
            Tracked source[4];
            source[0].value = 100;
            source[1].value = 101;
            source[2].value = 102;
            source[3].value = 103;

            destruction_sequence.clear();
            auto a = cc::array<Tracked>::create_copy_of(cc::span<Tracked const>(source));
        }

        // Last 4 entries should be the array destruction in reverse
        auto const n = destruction_sequence.size();
        CHECK(n >= 4);
        CHECK(destruction_sequence[n - 4] == 103);
        CHECK(destruction_sequence[n - 3] == 102);
        CHECK(destruction_sequence[n - 2] == 101);
        CHECK(destruction_sequence[n - 1] == 100);

        Tracked::destruction_order = nullptr;
    }

    SECTION("total ctors equals total dtors")
    {
        Tracked::reset_counters();
        {
            auto a = cc::array<Tracked>::create_defaulted(10);
            auto b = cc::array<Tracked>::create_filled(5, Tracked(42));
            auto c = a;
            auto d = cc::move(b);
        }

        auto const total_ctors = Tracked::default_ctor_count + Tracked::copy_ctor_count + Tracked::move_ctor_count;
        CHECK(total_ctors == Tracked::dtor_count);
    }
}
