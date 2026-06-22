#include <clean-core/span.hh>
#include <clean-core/utility.hh>
#include <clean-core/vector.hh>

#include <nexus/test.hh>

#include <optional>
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

TEST("vector - default construction invariants")
{
    SECTION("empty state - int")
    {
        cc::vector<int> v;
        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
        CHECK(v.begin() == v.end());
        CHECK(v.capacity() == 0);
        CHECK(v.capacity_back() == 0);
    }

    SECTION("empty state - Tracked")
    {
        Tracked::reset_counters();
        {
            cc::vector<Tracked> v;
            CHECK(v.size() == 0);
            CHECK(v.empty() == true);
            CHECK(v.begin() == v.end());
            CHECK(v.capacity() == 0);
        }
        CHECK(Tracked::default_ctor_count == 0);
        CHECK(Tracked::dtor_count == 0);
    }

    SECTION("iteration on empty vector")
    {
        cc::vector<int> v;
        int count = 0;
        for ([[maybe_unused]] auto const& val : v)
            ++count;
        CHECK(count == 0);
    }
}

TEST("vector - create_defaulted")
{
    SECTION("int - size 0")
    {
        auto v = cc::vector<int>::create_defaulted(0);
        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
        CHECK(v.capacity() == 0);
    }

    SECTION("int - size 1")
    {
        auto v = cc::vector<int>::create_defaulted(1);
        CHECK(v.size() == 1);
        CHECK(v.empty() == false);
        CHECK(v.front() == 0);
        CHECK(v.back() == 0);
        CHECK(v[0] == 0);
        CHECK(v.capacity() >= 1);
    }

    SECTION("int - size 5")
    {
        auto v = cc::vector<int>::create_defaulted(5);
        CHECK(v.size() == 5);
        CHECK(v.capacity() >= 5);
        for (int i = 0; i < 5; ++i)
            CHECK(v[i] == 0);
    }

    SECTION("Tracked - size 5")
    {
        Tracked::reset_counters();
        {
            auto v = cc::vector<Tracked>::create_defaulted(5);
            CHECK(v.size() == 5);
        }
        CHECK(Tracked::default_ctor_count == 5);
        CHECK(Tracked::dtor_count == 5);
    }
}

TEST("vector - create_filled")
{
    SECTION("int - size 0")
    {
        auto v = cc::vector<int>::create_filled(0, 7);
        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
    }

    SECTION("int - size 10")
    {
        auto v = cc::vector<int>::create_filled(10, 7);
        CHECK(v.size() == 10);
        for (int i = 0; i < 10; ++i)
            CHECK(v[i] == 7);
    }

    SECTION("TrackedCopy - size 10")
    {
        TrackedCopy::reset_counters();
        {
            auto const value = TrackedCopy(99);
            TrackedCopy::reset_counters();
            auto v = cc::vector<TrackedCopy>::create_filled(10, value);
            CHECK(v.size() == 10);
            for (int i = 0; i < 10; ++i)
                CHECK(v[i].value == 99);
        }
        CHECK(TrackedCopy::copy_ctor_count == 10);
    }
}

TEST("vector - create_uninitialized")
{
    SECTION("uint32_t - size 0")
    {
        auto v = cc::vector<uint32_t>::create_uninitialized(0);
        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
    }

    SECTION("uint32_t - size 128")
    {
        auto v = cc::vector<uint32_t>::create_uninitialized(128);
        CHECK(v.size() == 128);
        for (int i = 0; i < 128; ++i)
        {
            v[i] = static_cast<uint32_t>(i * 1000);
        }
        for (int i = 0; i < 128; ++i)
        {
            CHECK(v[i] == static_cast<uint32_t>(i * 1000));
        }
    }
}

TEST("vector - create_copy_of")
{
    SECTION("int - size 0")
    {
        auto v = cc::vector<int>::create_copy_of(cc::span<int const>{});
        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
    }

    SECTION("int - size 3")
    {
        int const source[] = {1, 2, 3};
        auto v = cc::vector<int>::create_copy_of(cc::span<int const>(source));
        CHECK(v.size() == 3);
        CHECK(v[0] == 1);
        CHECK(v[1] == 2);
        CHECK(v[2] == 3);
        CHECK(v.data() != source);
    }

    SECTION("TrackedCopy - size 3")
    {
        TrackedCopy const source[3] = {TrackedCopy(10), TrackedCopy(20), TrackedCopy(30)};
        TrackedCopy::reset_counters();

        auto v = cc::vector<TrackedCopy>::create_copy_of(cc::span<TrackedCopy const>(source));
        CHECK(v.size() == 3);
        CHECK(v[0].value == 10);
        CHECK(v[1].value == 20);
        CHECK(v[2].value == 30);
        CHECK(TrackedCopy::copy_ctor_count == 3);
    }
}

TEST("vector - create_with_capacity")
{
    SECTION("int - capacity 0")
    {
        auto v = cc::vector<int>::create_with_capacity(0);
        CHECK(v.size() == 0);
        CHECK(v.capacity() == 0);
    }

    SECTION("int - capacity 10")
    {
        auto v = cc::vector<int>::create_with_capacity(10);
        CHECK(v.size() == 0);
        CHECK(v.capacity() >= 10);
        CHECK(v.capacity_back() >= 10);
    }

    SECTION("Tracked - no construction with capacity")
    {
        Tracked::reset_counters();
        {
            auto v = cc::vector<Tracked>::create_with_capacity(20);
            CHECK(v.size() == 0);
            CHECK(v.capacity() >= 20);
        }
        CHECK(Tracked::default_ctor_count == 0);
        CHECK(Tracked::dtor_count == 0);
    }
}

TEST("vector - copy constructor deep copy")
{
    SECTION("int - basic deep copy")
    {
        auto v1 = cc::vector<int>::create_filled(5, 42);
        auto v2(v1);

        CHECK(v2.size() == 5);
        CHECK(v2.data() != v1.data());
        for (int i = 0; i < 5; ++i)
            CHECK(v2[i] == 42);

        v1[0] = 99;
        CHECK(v2[0] == 42);
    }

    SECTION("TrackedCopy - deep copy with unique objects")
    {
        auto v1 = cc::vector<TrackedCopy>::create_defaulted(3);
        v1[0].value = 10;
        v1[1].value = 20;
        v1[2].value = 30;

        TrackedCopy::reset_counters();
        auto v2(v1);

        CHECK(v2.size() == 3);
        CHECK(v2[0].value == 10);
        CHECK(v2.data() != v1.data());
        CHECK(TrackedCopy::copy_ctor_count == 3);
    }
}

TEST("vector - copy assignment keeps LHS resource")
{
    SECTION("different sizes and resources")
    {
        CountingResource resA;
        CountingResource resB;

        auto lhs = cc::vector<int>::create_defaulted(3, &resA);
        auto rhs = cc::vector<int>::create_defaulted(5, &resB);
        rhs[0] = 10;
        rhs[4] = 50;

        resA.reset();
        resB.reset();

        lhs = rhs;

        CHECK(lhs.size() == 5);
        CHECK(lhs[0] == 10);
        CHECK(lhs[4] == 50);

        CHECK(resA.allocations == 1);
        CHECK(resA.deallocations == 1);

        CHECK(resB.allocations == 0);
        CHECK(resB.deallocations == 0);
    }

    SECTION("self-assign does nothing")
    {
        CountingResource res;
        auto v = cc::vector<int>::create_defaulted(5, &res);
        res.reset();

        v = v;

        CHECK(v.size() == 5);
        CHECK(res.allocations == 0);
        CHECK(res.deallocations == 0);
    }
}

TEST("vector - move constructor transfers ownership")
{
    SECTION("int - size 0")
    {
        CountingResource res;
        auto v1 = cc::vector<int>::create_defaulted(0, &res);
        res.reset();

        auto v2 = cc::move(v1);

        CHECK(v2.size() == 0);
        CHECK(v1.size() == 0);
        CHECK(res.deallocations == 0);
    }

    SECTION("int - size 9")
    {
        CountingResource res;
        auto v1 = cc::vector<int>::create_defaulted(9, &res);
        for (int i = 0; i < 9; ++i)
            v1[i] = i * 10;
        res.reset();

        auto v2 = cc::move(v1);

        CHECK(v2.size() == 9);
        for (int i = 0; i < 9; ++i)
            CHECK(v2[i] == i * 10);
        CHECK(v1.size() == 0);
        CHECK(res.deallocations == 0);
    }
}

TEST("vector - move assignment transfers ownership")
{
    SECTION("different sizes and resources")
    {
        CountingResource resA;
        CountingResource resB;

        auto v1 = cc::vector<int>::create_defaulted(3, &resA);
        auto v2 = cc::vector<int>::create_defaulted(5, &resB);
        v2[0] = 10;
        v2[4] = 50;

        resA.reset();
        resB.reset();

        v1 = cc::move(v2);

        CHECK(v1.size() == 5);
        CHECK(v1[0] == 10);
        CHECK(v1[4] == 50);
        CHECK(v2.size() == 0);

        CHECK(resA.deallocations == 1);
        CHECK(resB.deallocations == 0);
    }
}

TEST("vector - move-only element type")
{
    SECTION("create_defaulted compiles")
    {
        auto v = cc::vector<MoveOnly>::create_defaulted(5);
        CHECK(v.size() == 5);
    }

    SECTION("move constructor compiles")
    {
        auto v1 = cc::vector<MoveOnly>::create_defaulted(5);
        auto v2 = cc::move(v1);
        CHECK(v2.size() == 5);
        CHECK(v1.size() == 0);
    }

    SECTION("move assignment compiles")
    {
        auto v1 = cc::vector<MoveOnly>::create_defaulted(5);
        auto v2 = cc::vector<MoveOnly>::create_defaulted(3);
        v2 = cc::move(v1);
        CHECK(v2.size() == 5);
        CHECK(v1.size() == 0);
    }
}

TEST("vector - iterators and data contiguity")
{
    SECTION("int - size 10")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        CHECK(v.end() - v.begin() == 10);
        CHECK(v.end() - v.begin() == v.size());

        for (int i = 0; i < 10; ++i)
            CHECK(&v[i] == v.data() + i);
    }

    SECTION("mutation through iteration")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        int value = 0;
        for (auto& elem : v)
            elem = value++;

        CHECK(v[0] == 0);
        CHECK(v[5] == 5);
        CHECK(v[9] == 9);
    }
}

TEST("vector - front/back/operator[] consistency")
{
    SECTION("int - size 1")
    {
        auto v = cc::vector<int>::create_filled(1, 99);
        CHECK(v.front() == 99);
        CHECK(v.back() == 99);
        CHECK(v[0] == 99);
    }

    SECTION("int - size > 1")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        v[0] = 100;
        v[9] = 200;

        CHECK(v.front() == 100);
        CHECK(v.back() == 200);
    }
}

TEST("vector - push_back basic growth")
{
    SECTION("int - single push")
    {
        cc::vector<int> v;
        v.push_back(42);

        CHECK(v.size() == 1);
        CHECK(v[0] == 42);
        CHECK(v.front() == 42);
        CHECK(v.back() == 42);
    }

    SECTION("int - multiple pushes")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i * 10);

        CHECK(v.size() == 10);
        for (int i = 0; i < 10; ++i)
            CHECK(v[i] == i * 10);
    }

    SECTION("TrackedCopy - copy on push")
    {
        TrackedCopy::reset_counters();
        {
            cc::vector<TrackedCopy> v;
            auto val = TrackedCopy(42);
            TrackedCopy::reset_counters();

            v.push_back(val);
            CHECK(v.size() == 1);
            CHECK(v[0].value == 42);
        }
        CHECK(TrackedCopy::copy_ctor_count >= 1);
    }

    SECTION("TrackedMove - move on push")
    {
        TrackedMove::reset_counters();
        {
            cc::vector<TrackedMove> v;
            v.push_back(TrackedMove(42));
            CHECK(v.size() == 1);
            CHECK(v[0].value == 42);
        }
        CHECK(TrackedMove::move_ctor_count >= 1);
    }
}

TEST("vector - emplace_back")
{
    SECTION("int - emplace values")
    {
        cc::vector<int> v;
        v.emplace_back(10);
        v.emplace_back(20);
        v.emplace_back(30);

        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[1] == 20);
        CHECK(v[2] == 30);
    }

    SECTION("Tracked - in-place construction")
    {
        Tracked::reset_counters();
        {
            cc::vector<Tracked> v;
            v.emplace_back(100);
            v.emplace_back(200);

            CHECK(v.size() == 2);
            CHECK(v[0].value == 100);
            CHECK(v[1].value == 200);
        }
        CHECK(Tracked::default_ctor_count == 2);
    }
}

TEST("vector - push_back with reallocation")
{
    SECTION("growth preserves existing elements")
    {
        CountingResource res;
        auto v = cc::vector<int>::create_with_resource(&res);

        for (int i = 0; i < 100; ++i)
            v.push_back(i);

        CHECK(v.size() == 100);
        for (int i = 0; i < 100; ++i)
            CHECK(v[i] == i);

        CHECK(res.allocations > 0);
    }

    SECTION("exponential growth reduces allocations")
    {
        CountingResource res;
        auto v = cc::vector<int>::create_with_resource(&res);
        res.reset();

        for (int i = 0; i < 1000; ++i)
            v.push_back(i);

        CHECK(v.size() == 1000);
        CHECK(res.allocations < 20);
    }
}

TEST("vector - push_back_stable requires capacity")
{
    SECTION("with sufficient capacity")
    {
        auto v = cc::vector<int>::create_with_capacity(10);
        v.push_back_stable(42);
        v.push_back_stable(99);

        CHECK(v.size() == 2);
        CHECK(v[0] == 42);
        CHECK(v[1] == 99);
    }

    SECTION("no reallocation")
    {
        CountingResource res;
        auto v = cc::vector<int>::create_with_capacity(10, &res);
        auto const original_data = v.data();
        res.reset();

        v.push_back_stable(1);
        v.push_back_stable(2);
        v.push_back_stable(3);

        CHECK(v.data() == original_data);
        CHECK(res.allocations == 0);
    }
}

TEST("vector - emplace_back_stable requires capacity")
{
    SECTION("with sufficient capacity")
    {
        auto v = cc::vector<Tracked>::create_with_capacity(10);
        v.emplace_back_stable(10);
        v.emplace_back_stable(20);

        CHECK(v.size() == 2);
        CHECK(v[0].value == 10);
        CHECK(v[1].value == 20);
    }
}

TEST("vector - pop_back")
{
    SECTION("int - single pop")
    {
        auto v = cc::vector<int>::create_filled(3, 42);
        auto val = v.pop_back();

        CHECK(val == 42);
        CHECK(v.size() == 2);
    }

    SECTION("int - multiple pops")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);

        CHECK(v.pop_back() == 30);
        CHECK(v.pop_back() == 20);
        CHECK(v.size() == 1);
        CHECK(v[0] == 10);
    }

    SECTION("Tracked - destruction on pop")
    {
        Tracked::reset_counters();
        {
            auto v = cc::vector<Tracked>::create_defaulted(5);
            auto const initial_dtor = Tracked::dtor_count;

            v.remove_back();
            CHECK(Tracked::dtor_count == initial_dtor + 1);
            CHECK(v.size() == 4);
        }
    }
}

TEST("vector - remove_back")
{
    SECTION("int - remove without return")
    {
        auto v = cc::vector<int>::create_filled(3, 42);
        v.remove_back();

        CHECK(v.size() == 2);
    }

    SECTION("multiple removes")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i);

        v.remove_back();
        v.remove_back();
        v.remove_back();

        CHECK(v.size() == 7);
        CHECK(v.back() == 6);
    }
}

TEST("vector - pop_at preserves order")
{
    SECTION("pop from middle")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(40);

        auto val = v.pop_at(1);

        CHECK(val == 20);
        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[1] == 30);
        CHECK(v[2] == 40);
    }

    SECTION("pop from front")
    {
        cc::vector<int> v;
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);

        auto val = v.pop_at(0);

        CHECK(val == 1);
        CHECK(v.size() == 2);
        CHECK(v[0] == 2);
        CHECK(v[1] == 3);
    }
}

TEST("vector - remove_at preserves order")
{
    SECTION("remove from middle")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(40);

        v.remove_at(2);

        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[1] == 20);
        CHECK(v[2] == 40);
    }
}

TEST("vector - pop_at_unordered")
{
    SECTION("pop from middle - O(1)")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(40);

        auto val = v.pop_at_unordered(1);

        CHECK(val == 20);
        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[2] == 30);
    }

    SECTION("pop last element")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);

        auto val = v.pop_at_unordered(2);

        CHECK(val == 30);
        CHECK(v.size() == 2);
        CHECK(v[0] == 10);
        CHECK(v[1] == 20);
    }
}

TEST("vector - remove_at_unordered")
{
    SECTION("remove from middle")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(40);

        v.remove_at_unordered(1);

        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
    }
}

TEST("vector - remove_at_range")
{
    SECTION("remove middle range")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i);

        v.remove_at_range(3, 4);

        CHECK(v.size() == 6);
        CHECK(v[0] == 0);
        CHECK(v[2] == 2);
        CHECK(v[3] == 7);
        CHECK(v[5] == 9);
    }

    SECTION("remove count 0")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);

        v.remove_at_range(1, 0);

        CHECK(v.size() == 2);
    }
}

TEST("vector - remove_from_to")
{
    SECTION("remove range [start, end)")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i);

        v.remove_from_to(2, 5);

        CHECK(v.size() == 7);
        CHECK(v[0] == 0);
        CHECK(v[1] == 1);
        CHECK(v[2] == 5);
        CHECK(v[3] == 6);
    }
}

TEST("vector - remove_at_range_unordered")
{
    SECTION("remove range unordered")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i);

        v.remove_at_range_unordered(2, 3);

        CHECK(v.size() == 7);
        CHECK(v[0] == 0);
        CHECK(v[1] == 1);
    }
}

TEST("vector - remove_from_to_unordered")
{
    SECTION("remove range [start, end) unordered")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i);

        v.remove_from_to_unordered(1, 4);

        CHECK(v.size() == 7);
        CHECK(v[0] == 0);
    }
}

TEST("vector - remove_first_where")
{
    SECTION("remove first matching element")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(20);

        auto removed_idx = v.remove_first_where([](int x) { return x == 20; });

        CHECK(removed_idx.has_value());
        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[1] == 30);
        CHECK(v[2] == 20);
    }

    SECTION("no match")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);

        auto removed_idx = v.remove_first_where([](int x) { return x == 99; });

        CHECK(!removed_idx.has_value());
        CHECK(v.size() == 2);
    }
}

TEST("vector - remove_last_where")
{
    SECTION("remove last matching element")
    {
        cc::vector<int> v;
        v.push_back(20);
        v.push_back(10);
        v.push_back(30);
        v.push_back(20);

        auto removed_idx = v.remove_last_where([](int x) { return x == 20; });

        CHECK(removed_idx.has_value());
        CHECK(v.size() == 3);
        CHECK(v[0] == 20);
        CHECK(v[1] == 10);
        CHECK(v[2] == 30);
    }
}

TEST("vector - remove_all_where")
{
    SECTION("remove all matching elements")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(20);
        v.push_back(40);

        auto count = v.remove_all_where([](int x) { return x == 20; });

        CHECK(count == 2);
        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[1] == 30);
        CHECK(v[2] == 40);
    }

    SECTION("no matches")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);

        auto count = v.remove_all_where([](int x) { return x == 99; });

        CHECK(count == 0);
        CHECK(v.size() == 2);
    }
}

TEST("vector - remove_first_value")
{
    SECTION("remove first occurrence")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(20);

        auto removed_idx = v.remove_first_value(20);

        CHECK(removed_idx.has_value());
        CHECK(v.size() == 3);
        CHECK(v[1] == 30);
    }
}

TEST("vector - remove_last_value")
{
    SECTION("remove last occurrence")
    {
        cc::vector<int> v;
        v.push_back(20);
        v.push_back(10);
        v.push_back(20);

        auto removed_idx = v.remove_last_value(20);

        CHECK(removed_idx.has_value());
        CHECK(v.size() == 2);
        CHECK(v[0] == 20);
        CHECK(v[1] == 10);
    }
}

TEST("vector - remove_all_value")
{
    SECTION("remove all occurrences")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(20);
        v.push_back(40);
        v.push_back(20);

        auto count = v.remove_all_value(20);

        CHECK(count == 3);
        CHECK(v.size() == 3);
        CHECK(v[0] == 10);
        CHECK(v[1] == 30);
        CHECK(v[2] == 40);
    }
}

TEST("vector - retain_all_where")
{
    SECTION("retain only even numbers")
    {
        cc::vector<int> v;
        for (int i = 0; i < 10; ++i)
            v.push_back(i);

        auto removed_count = v.retain_all_where([](int x) { return x % 2 == 0; });

        CHECK(removed_count == 5);
        CHECK(v.size() == 5);
        CHECK(v[0] == 0);
        CHECK(v[1] == 2);
        CHECK(v[4] == 8);
    }

    SECTION("retain all")
    {
        cc::vector<int> v;
        v.push_back(10);
        v.push_back(20);

        auto removed_count = v.retain_all_where([](int) { return true; });

        CHECK(removed_count == 0);
        CHECK(v.size() == 2);
    }
}

TEST("vector - resize_to_defaulted")
{
    SECTION("grow from empty")
    {
        cc::vector<int> v;
        v.resize_to_defaulted(5);

        CHECK(v.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(v[i] == 0);
    }

    SECTION("grow existing")
    {
        auto v = cc::vector<int>::create_filled(3, 42);
        v.resize_to_defaulted(7);

        CHECK(v.size() == 7);
        CHECK(v[0] == 42);
        CHECK(v[2] == 42);
        CHECK(v[3] == 0);
        CHECK(v[6] == 0);
    }

    SECTION("shrink")
    {
        auto v = cc::vector<int>::create_filled(10, 42);
        v.resize_to_defaulted(5);

        CHECK(v.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(v[i] == 42);
    }
}

TEST("vector - resize_to_filled")
{
    SECTION("grow with value")
    {
        cc::vector<int> v;
        v.resize_to_filled(5, 99);

        CHECK(v.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(v[i] == 99);
    }

    SECTION("grow existing with value")
    {
        auto v = cc::vector<int>::create_filled(3, 10);
        v.resize_to_filled(7, 99);

        CHECK(v.size() == 7);
        CHECK(v[0] == 10);
        CHECK(v[2] == 10);
        CHECK(v[3] == 99);
        CHECK(v[6] == 99);
    }
}

TEST("vector - resize_down_to")
{
    SECTION("shrink vector")
    {
        auto v = cc::vector<int>::create_filled(10, 42);
        v.resize_down_to(5);

        CHECK(v.size() == 5);
    }

    SECTION("Tracked - destruction on shrink")
    {
        Tracked::reset_counters();
        {
            auto v = cc::vector<Tracked>::create_defaulted(10);
            auto const initial_dtor = Tracked::dtor_count;

            v.resize_down_to(5);

            CHECK(Tracked::dtor_count == initial_dtor + 5);
            CHECK(v.size() == 5);
        }
    }
}

TEST("vector - resize_to_uninitialized")
{
    SECTION("grow with uninitialized memory")
    {
        cc::vector<uint32_t> v;
        v.resize_to_uninitialized(10);

        CHECK(v.size() == 10);

        for (int i = 0; i < 10; ++i)
            v[i] = static_cast<uint32_t>(i);

        for (int i = 0; i < 10; ++i)
            CHECK(v[i] == static_cast<uint32_t>(i));
    }
}

TEST("vector - clear_resize_to_defaulted")
{
    SECTION("clear and resize")
    {
        auto v = cc::vector<int>::create_filled(5, 42);
        v.clear_resize_to_defaulted(3);

        CHECK(v.size() == 3);
        for (int i = 0; i < 3; ++i)
            CHECK(v[i] == 0);
    }
}

TEST("vector - clear_resize_to_filled")
{
    SECTION("clear and resize with value")
    {
        auto v = cc::vector<int>::create_filled(5, 42);
        v.clear_resize_to_filled(3, 99);

        CHECK(v.size() == 3);
        for (int i = 0; i < 3; ++i)
            CHECK(v[i] == 99);
    }
}

TEST("vector - clear")
{
    SECTION("int - clear to empty")
    {
        auto v = cc::vector<int>::create_filled(10, 42);
        auto const old_capacity = v.capacity();

        v.clear();

        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
        CHECK(v.capacity() == old_capacity);
    }

    SECTION("Tracked - destructor called on clear")
    {
        Tracked::reset_counters();
        {
            auto v = cc::vector<Tracked>::create_defaulted(10);
            auto const initial_dtor = Tracked::dtor_count;

            v.clear();

            CHECK(Tracked::dtor_count == initial_dtor + 10);
            CHECK(v.size() == 0);
        }
    }
}

TEST("vector - fill")
{
    SECTION("fill existing elements")
    {
        auto v = cc::vector<int>::create_defaulted(5);
        v.fill(99);

        CHECK(v.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(v[i] == 99);
    }
}

TEST("vector - reserve")
{
    SECTION("reserve from empty")
    {
        cc::vector<int> v;
        v.reserve(100);

        CHECK(v.size() == 0);
        CHECK(v.capacity() >= 100);
    }

    SECTION("reserve increases capacity")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        v.reserve(200);

        CHECK(v.size() == 10);
        CHECK(v.capacity() >= 200);
    }

    SECTION("reserve does not decrease capacity")
    {
        auto v = cc::vector<int>::create_with_capacity(100);
        auto const old_capacity = v.capacity();

        v.reserve(50);

        CHECK(v.capacity() >= old_capacity);
    }

    SECTION("no allocation if sufficient capacity")
    {
        CountingResource res;
        auto v = cc::vector<int>::create_with_capacity(100, &res);
        res.reset();

        v.reserve(50);

        CHECK(res.allocations == 0);
    }
}

TEST("vector - reserve_exact")
{
    SECTION("exact allocation")
    {
        cc::vector<int> v;
        v.reserve_exact(100);

        CHECK(v.size() == 0);
        CHECK(v.capacity() >= 100);
    }
}

TEST("vector - reserve_back")
{
    SECTION("reserve back capacity")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        v.reserve_back(50);

        CHECK(v.size() == 10);
        CHECK(v.capacity_back() >= 50);
    }
}

TEST("vector - reserve_back_exact")
{
    SECTION("exact back capacity")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        v.reserve_back_exact(50);

        CHECK(v.size() == 10);
        CHECK(v.capacity_back() >= 50);
    }
}

TEST("vector - capacity")
{
    SECTION("capacity equals size + capacity_back")
    {
        auto v = cc::vector<int>::create_with_capacity(20);
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);

        CHECK(v.capacity() == v.size() + v.capacity_back());
    }

    SECTION("capacity after reserve")
    {
        cc::vector<int> v;
        v.reserve(100);

        CHECK(v.capacity() >= 100);
    }
}

TEST("vector - has_capacity_back_for")
{
    SECTION("check available capacity")
    {
        auto v = cc::vector<int>::create_with_capacity(20);

        CHECK(v.has_capacity_back_for(20) == true);
        CHECK(v.has_capacity_back_for(v.capacity()) == true);
        CHECK(v.has_capacity_back_for(v.capacity() + 1) == false);
    }
}

TEST("vector - shrink_to_fit")
{
    SECTION("shrink excess capacity")
    {
        auto v = cc::vector<int>::create_with_capacity(100);
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);

        v.shrink_to_fit();

        CHECK(v.size() == 3);
        CHECK(v.capacity() >= 3);
    }

    SECTION("empty vector shrink")
    {
        auto v = cc::vector<int>::create_with_capacity(100);
        v.shrink_to_fit();

        CHECK(v.size() == 0);
        CHECK(v.capacity() == 0);
    }
}

TEST("vector - create_from_allocation adopts without copying")
{
    SECTION("int - adopts allocation")
    {
        auto alloc = cc::allocation<int>::create_filled(5, 42, nullptr);
        auto const original_data = alloc.obj_start;

        auto v = cc::vector<int>::create_from_allocation(cc::move(alloc));

        CHECK(v.data() == original_data);
        CHECK(v.size() == 5);
        CHECK(v[0] == 42);
    }

    SECTION("no extra allocations")
    {
        CountingResource res;
        {
            auto alloc = cc::allocation<int>::create_defaulted(10, &res);
            res.reset();

            auto v = cc::vector<int>::create_from_allocation(cc::move(alloc));
            CHECK(v.size() == 10);

            CHECK(res.allocations == 0);
        }

        CHECK(res.deallocations == 1);
    }
}

TEST("vector - extract_allocation round-trip")
{
    SECTION("vector becomes empty but keeps resource")
    {
        CountingResource res;
        auto v = cc::vector<int>::create_defaulted(10, &res);
        for (int i = 0; i < 10; ++i)
            v[i] = i * 5;

        auto alloc = v.extract_allocation();

        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
        CHECK(alloc.obj_end - alloc.obj_start == 10);

        for (int i = 0; i < 10; ++i)
            CHECK(alloc.obj_start[i] == i * 5);
    }

    SECTION("round-trip preserves values")
    {
        auto v1 = cc::vector<int>::create_defaulted(5);
        v1[0] = 10;
        v1[4] = 50;

        auto alloc = v1.extract_allocation();
        CHECK(v1.size() == 0);

        auto v2 = cc::vector<int>::create_from_allocation(cc::move(alloc));
        CHECK(v2.size() == 5);
        CHECK(v2[0] == 10);
        CHECK(v2[4] == 50);
    }
}

TEST("vector - destruction order")
{
    SECTION("reverse destruction order")
    {
        std::vector<int> destruction_sequence;
        Tracked::reset_counters();
        Tracked::destruction_order = &destruction_sequence;

        {
            cc::vector<Tracked> v;
            v.emplace_back(0);
            v.emplace_back(1);
            v.emplace_back(2);
            v.emplace_back(3);
            v.emplace_back(4);
        }

        CHECK(destruction_sequence.size() == 5);
        CHECK(destruction_sequence[0] == 4);
        CHECK(destruction_sequence[1] == 3);
        CHECK(destruction_sequence[2] == 2);
        CHECK(destruction_sequence[3] == 1);
        CHECK(destruction_sequence[4] == 0);

        Tracked::destruction_order = nullptr;
    }

    SECTION("total ctors equals total dtors")
    {
        Tracked::reset_counters();
        {
            auto v1 = cc::vector<Tracked>::create_defaulted(10);
            auto v2 = cc::vector<Tracked>::create_filled(5, Tracked(42));
            auto v3 = v1;
            auto v4 = cc::move(v2);
        }

        auto const total_ctors = Tracked::default_ctor_count + Tracked::copy_ctor_count + Tracked::move_ctor_count;
        CHECK(total_ctors == Tracked::dtor_count);
    }
}

TEST("vector - allocation and deallocation balance")
{
    SECTION("total allocations equal deallocations")
    {
        CountingResource res;
        {
            auto v1 = cc::vector<int>::create_defaulted(10, &res);
            auto v2 = cc::vector<int>::create_with_capacity(20, &res);

            for (int i = 0; i < 100; ++i)
                v1.push_back(i);

            v2.resize_to_defaulted(50);
        }

        CHECK(res.allocations == res.deallocations);
        CHECK(res.total_allocated_bytes == res.total_deallocated_bytes);
    }
}

TEST("vector - size_bytes")
{
    SECTION("int - size_bytes calculation")
    {
        auto v = cc::vector<int>::create_defaulted(10);
        CHECK(v.size_bytes() == 10 * sizeof(int));
    }

    SECTION("empty vector")
    {
        cc::vector<int> v;
        CHECK(v.size_bytes() == 0);
    }
}

TEST("vector - initializer list construction")
{
    SECTION("int - from initializer list")
    {
        cc::vector<int> v = {1, 2, 3, 4, 5};

        CHECK(v.size() == 5);
        CHECK(v[0] == 1);
        CHECK(v[4] == 5);
    }

    SECTION("empty initializer list")
    {
        cc::vector<int> v = {};

        CHECK(v.size() == 0);
        CHECK(v.empty() == true);
    }
}
