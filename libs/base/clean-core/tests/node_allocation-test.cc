#include <clean-core/node_allocation.hh>
#include <clean-core/vector.hh>

#include <nexus/test.hh>

#include <array>

using namespace cc::primitive_defines;

namespace
{
// Test types covering different size/alignment classes

// 1B class (index 0, size 1)
struct T1B
{
    u8 value = 0;
    explicit T1B(u8 v = 0) : value(v) {}
};
static_assert(sizeof(T1B) == 1);
static_assert(alignof(T1B) == 1);

// 2B class (index 1, size 2)
struct T2B
{
    u16 value = 0;
    explicit T2B(u16 v = 0) : value(v) {}
};
static_assert(sizeof(T2B) == 2);
static_assert(alignof(T2B) == 2);

// 4B class (index 2, size 4)
struct T4B
{
    u32 value = 0;
    explicit T4B(u32 v = 0) : value(v) {}
};
static_assert(sizeof(T4B) == 4);
static_assert(alignof(T4B) == 4);

// 8B class (index 3, size 8)
struct T8B
{
    u64 value = 0;
    explicit T8B(u64 v = 0) : value(v) {}
};
static_assert(sizeof(T8B) == 8);
static_assert(alignof(T8B) == 8);

// 16B class (index 4, size 16)
struct T16B
{
    u64 a = 0;
    u64 b = 0;
    explicit T16B(u64 v = 0) : a(v), b(v * 2) {}
};
static_assert(sizeof(T16B) == 16);
static_assert(alignof(T16B) == 8);

// 32B class (index 5, size 32)
struct T32B
{
    u64 data[4] = {};
    explicit T32B(u64 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T32B) == 32);
static_assert(alignof(T32B) == 8);

// 64B class (index 6, size 64)
struct T64B
{
    u64 data[8] = {};
    explicit T64B(u64 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T64B) == 64);
static_assert(alignof(T64B) == 8);

// 128B class (index 7, size 128)
struct T128B
{
    u64 data[16] = {};
    explicit T128B(u64 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T128B) == 128);
static_assert(alignof(T128B) == 8);

// 256B class (index 8, size 256 - at small_max boundary)
struct T256B
{
    u64 data[32] = {};
    explicit T256B(u64 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T256B) == 256);
static_assert(alignof(T256B) == 8);

// Type with non-trivial destructor for tracking
struct TrackedDtor
{
    static inline int dtor_counter = 0;

    static void reset_counters() { dtor_counter = 0; }

    int value = 0;

    TrackedDtor() = default;
    explicit TrackedDtor(int v) : value(v) {}

    ~TrackedDtor() { ++dtor_counter; }

    TrackedDtor(TrackedDtor const&) = default;
    TrackedDtor(TrackedDtor&&) noexcept = default;
    TrackedDtor& operator=(TrackedDtor const&) = default;
    TrackedDtor& operator=(TrackedDtor&&) noexcept = default;
};

// Type requiring specific alignment
struct alignas(16) T16BAligned
{
    u64 a = 0;
    u64 b = 0;
    explicit T16BAligned(u64 v = 0) : a(v), b(v) {}
};
static_assert(sizeof(T16BAligned) == 16);
static_assert(alignof(T16BAligned) == 16);

// Trivial type verification
struct TrivialType
{
    int a = 0;
    int b = 0;
};
static_assert(std::is_trivially_copyable_v<TrivialType>);
static_assert(std::is_trivially_destructible_v<TrivialType>);

// Non-trivial type with complex destructor behavior
struct NonTrivialType
{
    static inline int ctor_counter = 0;
    static inline int dtor_counter = 0;

    static void reset_counters()
    {
        ctor_counter = 0;
        dtor_counter = 0;
    }

    int value = 0;

    NonTrivialType() = default;

    explicit NonTrivialType(int v) : value(v) { ++ctor_counter; }

    NonTrivialType(NonTrivialType const& other) : value(other.value) { ++ctor_counter; }

    NonTrivialType(NonTrivialType&& other) noexcept : value(other.value) { ++ctor_counter; }

    NonTrivialType& operator=(NonTrivialType const& other)
    {
        value = other.value;
        return *this;
    }

    NonTrivialType& operator=(NonTrivialType&& other) noexcept
    {
        value = other.value;
        return *this;
    }

    ~NonTrivialType() { ++dtor_counter; }
};
static_assert(!std::is_trivially_copyable_v<NonTrivialType>);
static_assert(!std::is_trivially_destructible_v<NonTrivialType>);

// Immovable type (non-copyable, non-movable)
struct ImmovableType
{
    int value = 0;
    int* constructed_at = nullptr;

    ImmovableType() = default;

    explicit ImmovableType(int v, int* addr = nullptr) : value(v), constructed_at(addr)
    {
        if (constructed_at)
            *constructed_at = value;
    }

    // Delete copy and move operations
    ImmovableType(ImmovableType const&) = delete;
    ImmovableType(ImmovableType&&) = delete;
    ImmovableType& operator=(ImmovableType const&) = delete;
    ImmovableType& operator=(ImmovableType&&) = delete;

    ~ImmovableType() = default;
};
static_assert(!std::is_copy_constructible_v<ImmovableType>);
static_assert(!std::is_move_constructible_v<ImmovableType>);

// Weird struct: 24B with align 8 (doesn't fit power-of-two perfectly)
// Will be allocated in 32B class (next power of 2)
struct T24B_Align8
{
    u64 a = 0;
    u64 b = 0;
    u64 c = 0; // 24 bytes total
    explicit T24B_Align8(u64 v = 0) : a(v), b(v * 2), c(v * 3) {}
};
static_assert(sizeof(T24B_Align8) == 24);
static_assert(alignof(T24B_Align8) == 8);

// Weird struct: 65B with align 1 (just over 64B boundary)
// Will be allocated in 128B class
struct alignas(1) T65B_Align1
{
    u8 data[65] = {};
    explicit T65B_Align1(u8 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T65B_Align1) == 65);
static_assert(alignof(T65B_Align1) == 1);

// Weird struct: 999B with align 2
// Will be allocated as large node (> 256B)
struct alignas(2) T999B_Align2
{
    u16 data[499] = {}; // 998 bytes, but struct will be 1000 due to alignment
    u8 extra = 0;
    explicit T999B_Align2(u16 v = 0) : extra(u8(v))
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T999B_Align2) == 1000);
static_assert(alignof(T999B_Align2) == 2);
} // namespace

TEST("node_allocation - basic single allocation")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("1B type")
    {
        auto node = cc::node_allocation<T1B>::create_from(alloc, 42);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->value == 42);
    }

    SECTION("2B type")
    {
        auto node = cc::node_allocation<T2B>::create_from(alloc, 123);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->value == 123);
    }

    SECTION("4B type")
    {
        auto node = cc::node_allocation<T4B>::create_from(alloc, 456);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->value == 456);
    }

    SECTION("8B type")
    {
        auto node = cc::node_allocation<T8B>::create_from(alloc, 789);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->value == 789);
    }

    SECTION("16B type")
    {
        auto node = cc::node_allocation<T16B>::create_from(alloc, 111);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->a == 111);
        CHECK(node.ptr->b == 222);
    }

    SECTION("32B type")
    {
        auto node = cc::node_allocation<T32B>::create_from(alloc, 333);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->data[0] == 333);
        CHECK(node.ptr->data[3] == 333);
    }

    SECTION("64B type")
    {
        auto node = cc::node_allocation<T64B>::create_from(alloc, 555);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->data[0] == 555);
        CHECK(node.ptr->data[7] == 555);
    }

    SECTION("128B type")
    {
        auto node = cc::node_allocation<T128B>::create_from(alloc, 777);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->data[0] == 777);
        CHECK(node.ptr->data[15] == 777);
    }

    SECTION("256B type")
    {
        auto node = cc::node_allocation<T256B>::create_from(alloc, 999);
        CHECK(node.is_valid());
        CHECK(node.ptr != nullptr);
        CHECK(node.ptr->data[0] == 999);
        CHECK(node.ptr->data[31] == 999);
    }
}

TEST("node_allocation - construction and destruction")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("default construction")
    {
        auto node = cc::node_allocation<T8B>::create_from(alloc);
        CHECK(node.is_valid());
        CHECK(node.ptr->value == 0);
    }

    SECTION("with arguments")
    {
        auto node = cc::node_allocation<T16B>::create_from(alloc, 42);
        CHECK(node.ptr->a == 42);
        CHECK(node.ptr->b == 84);
    }

    SECTION("destructor is called")
    {
        TrackedDtor::reset_counters();
        {
            auto node = cc::node_allocation<TrackedDtor>::create_from(alloc, 123);
            CHECK(node.ptr->value == 123);
            CHECK(TrackedDtor::dtor_counter == 0);
        }
        CHECK(TrackedDtor::dtor_counter == 1);
    }

    SECTION("multiple allocations with destructors")
    {
        TrackedDtor::reset_counters();
        {
            auto n1 = cc::node_allocation<TrackedDtor>::create_from(alloc, 1);
            auto n2 = cc::node_allocation<TrackedDtor>::create_from(alloc, 2);
            auto n3 = cc::node_allocation<TrackedDtor>::create_from(alloc, 3);
            CHECK(TrackedDtor::dtor_counter == 0);
        }
        CHECK(TrackedDtor::dtor_counter == 3);
    }
}

TEST("node_allocation - move semantics")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("move construction")
    {
        auto node1 = cc::node_allocation<T8B>::create_from(alloc, 42);
        auto const original_ptr = node1.ptr;

        auto node2 = cc::move(node1);

        CHECK(!node1.is_valid());
        CHECK(node1.ptr == nullptr);
        CHECK(node2.is_valid());
        CHECK(node2.ptr == original_ptr);
        CHECK(node2.ptr->value == 42);
    }

    SECTION("move assignment")
    {
        auto node1 = cc::node_allocation<T8B>::create_from(alloc, 42);
        auto const original_ptr = node1.ptr;

        auto node2 = cc::node_allocation<T8B>{};
        node2 = cc::move(node1);

        CHECK(!node1.is_valid());
        CHECK(node1.ptr == nullptr);
        CHECK(node2.is_valid());
        CHECK(node2.ptr == original_ptr);
        CHECK(node2.ptr->value == 42);
    }

    SECTION("move assignment frees previous value")
    {
        TrackedDtor::reset_counters();

        auto node1 = cc::node_allocation<TrackedDtor>::create_from(alloc, 1);
        auto node2 = cc::node_allocation<TrackedDtor>::create_from(alloc, 2);

        CHECK(TrackedDtor::dtor_counter == 0);

        node2 = cc::move(node1);
        node1 = {}; // ensure node1 is cleaned up

        // node2's original value should be destroyed
        CHECK(TrackedDtor::dtor_counter == 1);
        CHECK(node2.ptr->value == 1);
    }
}

TEST("node_allocation - validity checks")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("default constructed is invalid")
    {
        auto node = cc::node_allocation<T8B>{};
        CHECK(!node.is_valid());
        CHECK(!node);
        CHECK(node.ptr == nullptr);
    }

    SECTION("created node is valid")
    {
        auto node = cc::node_allocation<T8B>::create_from(alloc, 42);
        CHECK(node.is_valid());
        CHECK(static_cast<bool>(node));
        CHECK(node.ptr != nullptr);
    }

    SECTION("moved-from node is invalid")
    {
        auto node1 = cc::node_allocation<T8B>::create_from(alloc, 42);
        auto node2 = cc::move(node1);

        CHECK(!node1.is_valid());
        CHECK(!node1);
        CHECK(node2.is_valid());
        CHECK(static_cast<bool>(node2));
    }
}

TEST("node_allocation - pattern A: alloc all then free all - small scale")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("100 x 1B allocations")
    {
        std::array<cc::node_allocation<T1B>, 100> nodes;
        for (int i = 0; i < 100; ++i)
        {
            nodes[i] = cc::node_allocation<T1B>::create_from(alloc, u8(i));
            CHECK(nodes[i].is_valid());
        }

        // Verify all values
        for (int i = 0; i < 100; ++i)
        {
            CHECK(nodes[i].ptr->value == u8(i));
        }

        // All freed at scope exit
    }

    SECTION("100 x 4B allocations")
    {
        std::array<cc::node_allocation<T4B>, 100> nodes;
        for (int i = 0; i < 100; ++i)
        {
            nodes[i] = cc::node_allocation<T4B>::create_from(alloc, u32(i * 1000));
            CHECK(nodes[i].is_valid());
        }

        for (int i = 0; i < 100; ++i)
        {
            CHECK(nodes[i].ptr->value == u32(i * 1000));
        }
    }

    SECTION("100 x 16B allocations")
    {
        std::array<cc::node_allocation<T16B>, 100> nodes;
        for (int i = 0; i < 100; ++i)
        {
            nodes[i] = cc::node_allocation<T16B>::create_from(alloc, u64(i));
            CHECK(nodes[i].is_valid());
        }

        for (int i = 0; i < 100; ++i)
        {
            CHECK(nodes[i].ptr->a == u64(i));
        }
    }

    SECTION("100 x 64B allocations")
    {
        std::array<cc::node_allocation<T64B>, 100> nodes;
        for (int i = 0; i < 100; ++i)
        {
            nodes[i] = cc::node_allocation<T64B>::create_from(alloc, u64(i + 1000));
            CHECK(nodes[i].is_valid());
        }

        for (int i = 0; i < 100; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i + 1000));
        }
    }

    SECTION("100 x 256B allocations")
    {
        std::array<cc::node_allocation<T256B>, 100> nodes;
        for (int i = 0; i < 100; ++i)
        {
            nodes[i] = cc::node_allocation<T256B>::create_from(alloc, u64(i * 10));
            CHECK(nodes[i].is_valid());
        }

        for (int i = 0; i < 100; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i * 10));
        }
    }
}

TEST("node_allocation - pattern A: alloc all then free all - medium scale")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("1000 x 2B allocations")
    {
        cc::vector<cc::node_allocation<T2B>> nodes;
        for (int i = 0; i < 1000; ++i)
        {
            nodes.push_back(cc::node_allocation<T2B>::create_from(alloc, u16(i)));
            CHECK(nodes.back().is_valid());
        }

        for (int i = 0; i < 1000; ++i)
        {
            CHECK(nodes[i].ptr->value == u16(i));
        }
    }

    SECTION("1000 x 8B allocations")
    {
        cc::vector<cc::node_allocation<T8B>> nodes;
        for (int i = 0; i < 1000; ++i)
        {
            nodes.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i * 7)));
            CHECK(nodes.back().is_valid());
        }

        for (int i = 0; i < 1000; ++i)
        {
            CHECK(nodes[i].ptr->value == u64(i * 7));
        }
    }

    SECTION("1000 x 32B allocations")
    {
        cc::vector<cc::node_allocation<T32B>> nodes;
        for (int i = 0; i < 1000; ++i)
        {
            nodes.push_back(cc::node_allocation<T32B>::create_from(alloc, u64(i + 5000)));
        }

        for (int i = 0; i < 1000; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i + 5000));
        }
    }

    SECTION("1000 x 128B allocations")
    {
        cc::vector<cc::node_allocation<T128B>> nodes;
        for (int i = 0; i < 1000; ++i)
        {
            nodes.push_back(cc::node_allocation<T128B>::create_from(alloc, u64(i)));
        }

        for (int i = 0; i < 1000; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i));
        }
    }
}

TEST("node_allocation - pattern A: alloc all then free all - large scale")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("5000 x 8B allocations")
    {
        cc::vector<cc::node_allocation<T8B>> nodes;
        for (int i = 0; i < 5000; ++i)
        {
            nodes.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i)));
        }

        for (int i = 0; i < 5000; ++i)
        {
            CHECK(nodes[i].ptr->value == u64(i));
        }
    }

    SECTION("5000 x 64B allocations")
    {
        cc::vector<cc::node_allocation<T64B>> nodes;
        for (int i = 0; i < 5000; ++i)
        {
            nodes.push_back(cc::node_allocation<T64B>::create_from(alloc, u64(i * 3)));
        }

        // Spot check instead of verifying all 5000
        for (int i = 0; i < 5000; i += 100)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i * 3));
        }
    }

    SECTION("3000 x 128B allocations")
    {
        cc::vector<cc::node_allocation<T128B>> nodes;
        for (int i = 0; i < 3000; ++i)
        {
            nodes.push_back(cc::node_allocation<T128B>::create_from(alloc, u64(i + 10000)));
        }

        for (int i = 0; i < 3000; i += 50)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i + 10000));
        }
    }
}

TEST("node_allocation - pattern B: interleaved alloc/dealloc - small active set")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("1000 iterations, ~10 alive - 1B")
    {
        std::array<cc::node_allocation<T1B>, 10> active;

        for (int iter = 0; iter < 1000; ++iter)
        {
            int const idx = iter % 10;

            // Free existing if present
            active[idx] = cc::node_allocation<T1B>{};

            // Allocate new
            active[idx] = cc::node_allocation<T1B>::create_from(alloc, u8(iter));
            CHECK(active[idx].is_valid());
        }

        // Verify last 10
        for (int i = 0; i < 10; ++i)
        {
            int const expected_iter = 990 + i;
            CHECK(active[i].ptr->value == u8(expected_iter));
        }
    }

    SECTION("1000 iterations, ~10 alive - 4B")
    {
        std::array<cc::node_allocation<T4B>, 10> active;

        for (int iter = 0; iter < 1000; ++iter)
        {
            int const idx = iter % 10;
            active[idx] = cc::node_allocation<T4B>::create_from(alloc, u32(iter * 100));
            CHECK(active[idx].is_valid());
        }

        for (int i = 0; i < 10; ++i)
        {
            int const expected_iter = 990 + i;
            CHECK(active[i].ptr->value == u32(expected_iter * 100));
        }
    }

    SECTION("2000 iterations, ~10 alive - 16B")
    {
        std::array<cc::node_allocation<T16B>, 10> active;

        for (int iter = 0; iter < 2000; ++iter)
        {
            int const idx = iter % 10;
            active[idx] = cc::node_allocation<T16B>::create_from(alloc, u64(iter));
            CHECK(active[idx].is_valid());
        }

        for (int i = 0; i < 10; ++i)
        {
            int const expected_iter = 1990 + i;
            CHECK(active[i].ptr->a == u64(expected_iter));
        }
    }

    SECTION("1500 iterations, ~10 alive - 64B")
    {
        std::array<cc::node_allocation<T64B>, 10> active;

        for (int iter = 0; iter < 1500; ++iter)
        {
            int const idx = iter % 10;
            active[idx] = cc::node_allocation<T64B>::create_from(alloc, u64(iter + 999));
        }

        for (int i = 0; i < 10; ++i)
        {
            int const expected_iter = 1490 + i;
            CHECK(active[i].ptr->data[0] == u64(expected_iter + 999));
        }
    }
}

TEST("node_allocation - pattern B: interleaved alloc/dealloc - medium active set")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("2000 iterations, ~50 alive - 2B")
    {
        std::array<cc::node_allocation<T2B>, 50> active;

        for (int iter = 0; iter < 2000; ++iter)
        {
            int const idx = iter % 50;
            active[idx] = cc::node_allocation<T2B>::create_from(alloc, u16(iter));
        }

        for (int i = 0; i < 50; ++i)
        {
            int const expected_iter = 1950 + i;
            CHECK(active[i].ptr->value == u16(expected_iter));
        }
    }

    SECTION("3000 iterations, ~50 alive - 8B")
    {
        std::array<cc::node_allocation<T8B>, 50> active;

        for (int iter = 0; iter < 3000; ++iter)
        {
            int const idx = iter % 50;
            active[idx] = cc::node_allocation<T8B>::create_from(alloc, u64(iter * 13));
        }

        for (int i = 0; i < 50; ++i)
        {
            int const expected_iter = 2950 + i;
            CHECK(active[i].ptr->value == u64(expected_iter * 13));
        }
    }

    SECTION("2000 iterations, ~50 alive - 32B")
    {
        std::array<cc::node_allocation<T32B>, 50> active;

        for (int iter = 0; iter < 2000; ++iter)
        {
            int const idx = iter % 50;
            active[idx] = cc::node_allocation<T32B>::create_from(alloc, u64(iter));
        }

        for (int i = 0; i < 50; ++i)
        {
            int const expected_iter = 1950 + i;
            CHECK(active[i].ptr->data[0] == u64(expected_iter));
        }
    }

    SECTION("1500 iterations, ~50 alive - 128B")
    {
        std::array<cc::node_allocation<T128B>, 50> active;

        for (int iter = 0; iter < 1500; ++iter)
        {
            int const idx = iter % 50;
            active[idx] = cc::node_allocation<T128B>::create_from(alloc, u64(iter + 7777));
        }

        for (int i = 0; i < 50; ++i)
        {
            int const expected_iter = 1450 + i;
            CHECK(active[i].ptr->data[0] == u64(expected_iter + 7777));
        }
    }
}

TEST("node_allocation - pattern B: interleaved alloc/dealloc - large active set")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("3000 iterations, ~100 alive - 1B")
    {
        std::array<cc::node_allocation<T1B>, 100> active;

        for (int iter = 0; iter < 3000; ++iter)
        {
            int const idx = iter % 100;
            active[idx] = cc::node_allocation<T1B>::create_from(alloc, u8(iter));
        }

        for (int i = 0; i < 100; ++i)
        {
            int const expected_iter = 2900 + i;
            CHECK(active[i].ptr->value == u8(expected_iter));
        }
    }

    SECTION("5000 iterations, ~100 alive - 8B")
    {
        std::array<cc::node_allocation<T8B>, 100> active;

        for (int iter = 0; iter < 5000; ++iter)
        {
            int const idx = iter % 100;
            active[idx] = cc::node_allocation<T8B>::create_from(alloc, u64(iter * 11));
        }

        for (int i = 0; i < 100; ++i)
        {
            int const expected_iter = 4900 + i;
            CHECK(active[i].ptr->value == u64(expected_iter * 11));
        }
    }

    SECTION("2500 iterations, ~100 alive - 64B")
    {
        std::array<cc::node_allocation<T64B>, 100> active;

        for (int iter = 0; iter < 2500; ++iter)
        {
            int const idx = iter % 100;
            active[idx] = cc::node_allocation<T64B>::create_from(alloc, u64(iter));
        }

        for (int i = 0; i < 100; ++i)
        {
            int const expected_iter = 2400 + i;
            CHECK(active[i].ptr->data[0] == u64(expected_iter));
        }
    }

    SECTION("2000 iterations, ~100 alive - 256B")
    {
        std::array<cc::node_allocation<T256B>, 100> active;

        for (int iter = 0; iter < 2000; ++iter)
        {
            int const idx = iter % 100;
            active[idx] = cc::node_allocation<T256B>::create_from(alloc, u64(iter + 5555));
        }

        for (int i = 0; i < 100; ++i)
        {
            int const expected_iter = 1900 + i;
            CHECK(active[i].ptr->data[0] == u64(expected_iter + 5555));
        }
    }
}

TEST("node_allocation - pattern C: alloc all, dealloc in reverse order")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("500 x 4B - reverse dealloc")
    {
        cc::vector<cc::node_allocation<T4B>> nodes;

        // Allocate all
        for (int i = 0; i < 500; ++i)
        {
            nodes.push_back(cc::node_allocation<T4B>::create_from(alloc, u32(i)));
        }

        // Verify all
        for (int i = 0; i < 500; ++i)
        {
            CHECK(nodes[i].ptr->value == u32(i));
        }

        // Deallocate in reverse order
        for (int i = 499; i >= 0; --i)
        {
            nodes[i] = cc::node_allocation<T4B>{};
        }
    }

    SECTION("300 x 16B - reverse dealloc")
    {
        cc::vector<cc::node_allocation<T16B>> nodes;

        for (int i = 0; i < 300; ++i)
        {
            nodes.push_back(cc::node_allocation<T16B>::create_from(alloc, u64(i * 2)));
        }

        for (int i = 299; i >= 0; --i)
        {
            CHECK(nodes[i].ptr->a == u64(i * 2));
            nodes[i] = cc::node_allocation<T16B>{};
        }
    }

    SECTION("200 x 128B - reverse dealloc")
    {
        cc::vector<cc::node_allocation<T128B>> nodes;

        for (int i = 0; i < 200; ++i)
        {
            nodes.push_back(cc::node_allocation<T128B>::create_from(alloc, u64(i + 1000)));
        }

        for (int i = 199; i >= 0; --i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i + 1000));
            nodes[i] = cc::node_allocation<T128B>{};
        }
    }
}

TEST("node_allocation - pattern C: alloc all, dealloc in forward order")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("500 x 2B - forward dealloc")
    {
        cc::vector<cc::node_allocation<T2B>> nodes;

        for (int i = 0; i < 500; ++i)
        {
            nodes.push_back(cc::node_allocation<T2B>::create_from(alloc, u16(i)));
        }

        // Deallocate in forward order
        for (int i = 0; i < 500; ++i)
        {
            CHECK(nodes[i].ptr->value == u16(i));
            nodes[i] = cc::node_allocation<T2B>{};
        }
    }

    SECTION("400 x 32B - forward dealloc")
    {
        cc::vector<cc::node_allocation<T32B>> nodes;

        for (int i = 0; i < 400; ++i)
        {
            nodes.push_back(cc::node_allocation<T32B>::create_from(alloc, u64(i * 3)));
        }

        for (int i = 0; i < 400; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i * 3));
            nodes[i] = cc::node_allocation<T32B>{};
        }
    }

    SECTION("250 x 256B - forward dealloc")
    {
        cc::vector<cc::node_allocation<T256B>> nodes;

        for (int i = 0; i < 250; ++i)
        {
            nodes.push_back(cc::node_allocation<T256B>::create_from(alloc, u64(i + 8888)));
        }

        for (int i = 0; i < 250; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i + 8888));
            nodes[i] = cc::node_allocation<T256B>{};
        }
    }
}

TEST("node_allocation - pattern C: alloc all, dealloc alternating pattern")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("400 x 8B - alternating dealloc (evens then odds)")
    {
        cc::vector<cc::node_allocation<T8B>> nodes;

        for (int i = 0; i < 400; ++i)
        {
            nodes.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i * 7)));
        }

        // Free all evens
        for (int i = 0; i < 400; i += 2)
        {
            CHECK(nodes[i].ptr->value == u64(i * 7));
            nodes[i] = cc::node_allocation<T8B>{};
        }

        // Verify odds still valid
        for (int i = 1; i < 400; i += 2)
        {
            CHECK(nodes[i].is_valid());
            CHECK(nodes[i].ptr->value == u64(i * 7));
        }

        // Free all odds
        for (int i = 1; i < 400; i += 2)
        {
            nodes[i] = cc::node_allocation<T8B>{};
        }
    }

    SECTION("300 x 64B - alternating dealloc (thirds)")
    {
        cc::vector<cc::node_allocation<T64B>> nodes;

        for (int i = 0; i < 300; ++i)
        {
            nodes.push_back(cc::node_allocation<T64B>::create_from(alloc, u64(i)));
        }

        // Free every third starting at 0
        for (int i = 0; i < 300; i += 3)
        {
            nodes[i] = cc::node_allocation<T64B>{};
        }

        // Free every third starting at 1
        for (int i = 1; i < 300; i += 3)
        {
            CHECK(nodes[i].ptr->data[0] == u64(i));
            nodes[i] = cc::node_allocation<T64B>{};
        }

        // Verify remaining (every third starting at 2) and free
        for (int i = 2; i < 300; i += 3)
        {
            CHECK(nodes[i].is_valid());
            CHECK(nodes[i].ptr->data[0] == u64(i));
            nodes[i] = cc::node_allocation<T64B>{};
        }
    }
}

TEST("node_allocation - alignment requirements")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("16-byte aligned type")
    {
        auto node = cc::node_allocation<T16BAligned>::create_from(alloc, 42);
        CHECK(node.is_valid());
        CHECK(node.ptr->a == 42);

        // Verify alignment
        auto const addr = reinterpret_cast<std::uintptr_t>(node.ptr);
        CHECK(addr % 16 == 0);
    }

    SECTION("multiple 16-byte aligned allocations")
    {
        std::array<cc::node_allocation<T16BAligned>, 50> nodes;

        for (int i = 0; i < 50; ++i)
        {
            nodes[i] = cc::node_allocation<T16BAligned>::create_from(alloc, u64(i));
            CHECK(nodes[i].is_valid());

            auto const addr = reinterpret_cast<std::uintptr_t>(nodes[i].ptr);
            CHECK(addr % 16 == 0);
        }
    }
}

TEST("node_allocation - second slab")
{
    auto& alloc = cc::default_node_allocator();

    cc::vector<cc::node_allocation<u64>> nodes;

    for (int i = 0; i < 100; ++i)
        nodes.push_back(cc::node_allocation<u64>::create_from(alloc, i * 2 + 1));

    for (int i = 0; i < 100; ++i)
        CHECK(*nodes[i].ptr == i * 2 + 1);
}

TEST("node_allocation - several slabs")
{
    auto& alloc = cc::default_node_allocator();

    cc::vector<cc::node_allocation<u64>> nodes;

    for (int i = 0; i < 200; ++i)
        nodes.push_back(cc::node_allocation<u64>::create_from(alloc, i * 2 + 1));

    for (int i = 0; i < 200; ++i)
        CHECK(*nodes[i].ptr == i * 2 + 1);
}

TEST("node_allocation - mixed size class stress test")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("interleaved allocations of different sizes")
    {
        cc::vector<cc::node_allocation<T1B>> n1b;
        cc::vector<cc::node_allocation<T8B>> n8b;
        cc::vector<cc::node_allocation<T64B>> n64b;
        cc::vector<cc::node_allocation<T256B>> n256b;

        for (int i = 0; i < 200; ++i)
        {
            n1b.push_back(cc::node_allocation<T1B>::create_from(alloc, u8(i)));
            n8b.push_back(cc::node_allocation<T8B>::create_from(alloc, u64(i * 100)));
            n64b.push_back(cc::node_allocation<T64B>::create_from(alloc, u64(i * 1000)));
            n256b.push_back(cc::node_allocation<T256B>::create_from(alloc, u64(i * 10000)));
        }

        // Verify all allocations
        for (int i = 0; i < 200; ++i)
        {
            CHECK(n1b[i].ptr->value == u8(i));
            CHECK(n8b[i].ptr->value == u64(i * 100));
            CHECK(n64b[i].ptr->data[0] == u64(i * 1000));
            CHECK(n256b[i].ptr->data[0] == u64(i * 10000));
        }

        // Free in mixed order (free some 64B first, then 1B, etc.)
        for (int i = 0; i < 100; ++i)
        {
            n64b[i] = cc::node_allocation<T64B>{};
        }

        for (int i = 0; i < 50; ++i)
        {
            n1b[i] = cc::node_allocation<T1B>{};
        }

        // Verify remaining are still valid
        for (int i = 50; i < 200; ++i)
        {
            CHECK(n1b[i].is_valid());
            CHECK(n1b[i].ptr->value == u8(i));
        }

        for (int i = 100; i < 200; ++i)
        {
            CHECK(n64b[i].is_valid());
            CHECK(n64b[i].ptr->data[0] == u64(i * 1000));
        }
    }
}

TEST("node_allocation - trivial types")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("trivial type basic operations")
    {
        auto node = cc::node_allocation<TrivialType>::create_from(alloc);
        CHECK(node.is_valid());
        CHECK(node.ptr->a == 0);
        CHECK(node.ptr->b == 0);

        node.ptr->a = 42;
        node.ptr->b = 99;

        CHECK(node.ptr->a == 42);
        CHECK(node.ptr->b == 99);
    }

    SECTION("many trivial allocations")
    {
        cc::vector<cc::node_allocation<TrivialType>> nodes;
        for (int i = 0; i < 500; ++i)
        {
            nodes.push_back(cc::node_allocation<TrivialType>::create_from(alloc));
            nodes.back().ptr->a = i;
            nodes.back().ptr->b = i * 2;
        }

        for (int i = 0; i < 500; ++i)
        {
            CHECK(nodes[i].ptr->a == i);
            CHECK(nodes[i].ptr->b == i * 2);
        }
    }
}

TEST("node_allocation - non-trivial types")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("non-trivial construction and destruction")
    {
        NonTrivialType::reset_counters();

        {
            auto node = cc::node_allocation<NonTrivialType>::create_from(alloc, 42);
            CHECK(node.is_valid());
            CHECK(node.ptr->value == 42);
            CHECK(NonTrivialType::ctor_counter > 0);  // At least one ctor was called
            CHECK(NonTrivialType::dtor_counter == 0); // Not yet destroyed
        }

        CHECK(NonTrivialType::dtor_counter > 0); // Destructor was called
    }

    SECTION("many non-trivial allocations")
    {
        NonTrivialType::reset_counters();

        {
            cc::vector<cc::node_allocation<NonTrivialType>> nodes;
            for (int i = 0; i < 300; ++i)
            {
                nodes.push_back(cc::node_allocation<NonTrivialType>::create_from(alloc, i));
            }

            CHECK(NonTrivialType::ctor_counter > 0);
            CHECK(NonTrivialType::dtor_counter == 0); // None destroyed yet

            // Verify values
            for (int i = 0; i < 300; ++i)
            {
                CHECK(nodes[i].ptr->value == i);
            }

            int const dtor_before_manual_clear = NonTrivialType::dtor_counter;

            // Manually destroy some
            for (int i = 0; i < 100; ++i)
            {
                nodes[i] = cc::node_allocation<NonTrivialType>{};
            }

            CHECK(NonTrivialType::dtor_counter > dtor_before_manual_clear); // Some dtors called
        }

        // All should be destroyed now
        CHECK(NonTrivialType::dtor_counter >= 300);
    }

    SECTION("non-trivial interleaved pattern")
    {
        NonTrivialType::reset_counters();

        std::array<cc::node_allocation<NonTrivialType>, 20> active;

        for (int iter = 0; iter < 500; ++iter)
        {
            int const idx = iter % 20;
            int const prev_dtor = NonTrivialType::dtor_counter;

            active[idx] = cc::node_allocation<NonTrivialType>::create_from(alloc, iter);

            if (iter >= 20)
            {
                // Previous value should have been destroyed
                CHECK(NonTrivialType::dtor_counter > prev_dtor);
            }
        }

        // Verify final values
        for (int i = 0; i < 20; ++i)
        {
            int const expected_iter = 480 + i;
            CHECK(active[i].ptr->value == expected_iter);
        }
    }
}

TEST("node_allocation - immovable types")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("immovable type construction in place")
    {
        int constructed_value = 0;

        auto node = cc::node_allocation<ImmovableType>::create_from(alloc, 42, &constructed_value);

        CHECK(node.is_valid());
        CHECK(node.ptr->value == 42);
        CHECK(constructed_value == 42); // Constructed in place

        // Verify we can access the object
        node.ptr->value = 99;
        CHECK(node.ptr->value == 99);
    }

    SECTION("many immovable allocations")
    {
        cc::vector<cc::node_allocation<ImmovableType>> nodes;

        for (int i = 0; i < 200; ++i)
        {
            nodes.push_back(cc::node_allocation<ImmovableType>::create_from(alloc, i, nullptr));
            CHECK(nodes.back().ptr->value == i);
        }

        // Verify all
        for (int i = 0; i < 200; ++i)
        {
            CHECK(nodes[i].ptr->value == i);
        }

        // Modify in place (can't move)
        for (int i = 0; i < 200; ++i)
        {
            nodes[i].ptr->value = i * 10;
        }

        // Verify modifications
        for (int i = 0; i < 200; ++i)
        {
            CHECK(nodes[i].ptr->value == i * 10);
        }
    }

    SECTION("immovable type can only be moved via node_allocation move")
    {
        auto node1 = cc::node_allocation<ImmovableType>::create_from(alloc, 123, nullptr);
        auto const original_ptr = node1.ptr;

        // Move the node_allocation itself (not the contained object)
        auto node2 = cc::move(node1);

        CHECK(!node1.is_valid());
        CHECK(node2.is_valid());
        CHECK(node2.ptr == original_ptr); // Same object, no copy/move of ImmovableType
        CHECK(node2.ptr->value == 123);
    }

    SECTION("immovable interleaved pattern")
    {
        std::array<cc::node_allocation<ImmovableType>, 15> active;

        for (int iter = 0; iter < 1000; ++iter)
        {
            int const idx = iter % 15;
            active[idx] = cc::node_allocation<ImmovableType>::create_from(alloc, iter, nullptr);
        }

        for (int i = 0; i < 15; ++i)
        {
            int const expected_iter = 985 + i;
            CHECK(active[expected_iter % 15].ptr->value == expected_iter);
        }
    }
}

TEST("node_allocation - weird struct sizes")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("24B align 8 - single allocation")
    {
        auto node = cc::node_allocation<T24B_Align8>::create_from(alloc, 123);
        CHECK(node.is_valid());
        CHECK(node.ptr->a == 123);
        CHECK(node.ptr->b == 246);
        CHECK(node.ptr->c == 369);

        // Verify alignment
        auto const addr = reinterpret_cast<std::uintptr_t>(node.ptr);
        CHECK(addr % 8 == 0);
    }

    SECTION("24B align 8 - many allocations")
    {
        cc::vector<cc::node_allocation<T24B_Align8>> nodes;
        for (int i = 0; i < 400; ++i)
        {
            nodes.push_back(cc::node_allocation<T24B_Align8>::create_from(alloc, u64(i)));
        }

        for (int i = 0; i < 400; ++i)
        {
            CHECK(nodes[i].ptr->a == u64(i));
            CHECK(nodes[i].ptr->b == u64(i * 2));
            CHECK(nodes[i].ptr->c == u64(i * 3));

            // Verify alignment for each
            auto const addr = reinterpret_cast<std::uintptr_t>(nodes[i].ptr);
            CHECK(addr % 8 == 0);
        }
    }

    SECTION("24B align 8 - interleaved pattern")
    {
        std::array<cc::node_allocation<T24B_Align8>, 30> active;

        for (int iter = 0; iter < 1500; ++iter)
        {
            int const idx = iter % 30;
            active[idx] = cc::node_allocation<T24B_Align8>::create_from(alloc, u64(iter));
        }

        for (int i = 0; i < 30; ++i)
        {
            int const expected = 1470 + i;
            CHECK(active[i].ptr->a == u64(expected));
        }
    }

    SECTION("65B align 1 - single allocation")
    {
        auto node = cc::node_allocation<T65B_Align1>::create_from(alloc, 42);
        CHECK(node.is_valid());
        CHECK(node.ptr->data[0] == 42);
        CHECK(node.ptr->data[64] == 42);

        // Alignment is 1, so any address is fine (but will be allocated in 128B class)
    }

    SECTION("65B align 1 - many allocations")
    {
        cc::vector<cc::node_allocation<T65B_Align1>> nodes;
        for (int i = 0; i < 300; ++i)
        {
            nodes.push_back(cc::node_allocation<T65B_Align1>::create_from(alloc, u8(i)));
        }

        for (int i = 0; i < 300; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u8(i));
            CHECK(nodes[i].ptr->data[32] == u8(i));
            CHECK(nodes[i].ptr->data[64] == u8(i));
        }
    }

    SECTION("65B align 1 - interleaved pattern")
    {
        std::array<cc::node_allocation<T65B_Align1>, 25> active;

        for (int iter = 0; iter < 1000; ++iter)
        {
            int const idx = iter % 25;
            active[idx] = cc::node_allocation<T65B_Align1>::create_from(alloc, u8(iter));
        }

        for (int i = 0; i < 25; ++i)
        {
            int const expected = 975 + i;
            CHECK(active[i].ptr->data[0] == u8(expected));
        }
    }

    SECTION("999B align 2 - single allocation (large node)")
    {
        auto node = cc::node_allocation<T999B_Align2>::create_from(alloc, 77);
        CHECK(node.is_valid());
        CHECK(node.ptr->data[0] == 77);
        CHECK(node.ptr->data[498] == 77);
        CHECK(node.ptr->extra == 77);

        // Verify alignment
        auto const addr = reinterpret_cast<std::uintptr_t>(node.ptr);
        CHECK(addr % 2 == 0);
    }

    SECTION("999B align 2 - many allocations (large nodes)")
    {
        cc::vector<cc::node_allocation<T999B_Align2>> nodes;
        for (int i = 0; i < 100; ++i)
        {
            nodes.push_back(cc::node_allocation<T999B_Align2>::create_from(alloc, u16(i + 1000)));
        }

        for (int i = 0; i < 100; ++i)
        {
            CHECK(nodes[i].ptr->data[0] == u16(i + 1000));
            CHECK(nodes[i].ptr->data[498] == u16(i + 1000));
            CHECK(nodes[i].ptr->extra == u8(i + 1000));

            auto const addr = reinterpret_cast<std::uintptr_t>(nodes[i].ptr);
            CHECK(addr % 2 == 0);
        }
    }

    SECTION("999B align 2 - interleaved pattern")
    {
        std::array<cc::node_allocation<T999B_Align2>, 10> active;

        for (int iter = 0; iter < 500; ++iter)
        {
            int const idx = iter % 10;
            active[idx] = cc::node_allocation<T999B_Align2>::create_from(alloc, u16(iter));
        }

        for (int i = 0; i < 10; ++i)
        {
            int const expected = 490 + i;
            CHECK(active[i].ptr->data[0] == u16(expected));
            CHECK(active[i].ptr->extra == u8(expected));
        }
    }

    SECTION("mixed weird sizes together")
    {
        cc::vector<cc::node_allocation<T24B_Align8>> n24;
        cc::vector<cc::node_allocation<T65B_Align1>> n65;
        cc::vector<cc::node_allocation<T999B_Align2>> n999;

        for (int i = 0; i < 150; ++i)
        {
            n24.push_back(cc::node_allocation<T24B_Align8>::create_from(alloc, u64(i)));
            n65.push_back(cc::node_allocation<T65B_Align1>::create_from(alloc, u8(i + 100)));

            // Fewer large allocations
            if (i < 50)
                n999.push_back(cc::node_allocation<T999B_Align2>::create_from(alloc, u16(i + 200)));
        }

        // Verify all
        for (int i = 0; i < 150; ++i)
        {
            CHECK(n24[i].ptr->a == u64(i));
            CHECK(n65[i].ptr->data[0] == u8(i + 100));
        }

        for (int i = 0; i < 50; ++i)
        {
            CHECK(n999[i].ptr->data[0] == u16(i + 200));
        }

        // Free some 24B
        for (int i = 0; i < 50; ++i)
        {
            n24[i] = cc::node_allocation<T24B_Align8>{};
        }

        // Verify remaining are valid
        for (int i = 50; i < 150; ++i)
        {
            CHECK(n24[i].is_valid());
            CHECK(n24[i].ptr->a == u64(i));
        }
    }
}

TEST("node_allocation - weird sizes with all patterns")
{
    auto& alloc = cc::default_node_allocator();

    SECTION("pattern A: 24B align 8 - alloc all, free all")
    {
        cc::vector<cc::node_allocation<T24B_Align8>> nodes;
        for (int i = 0; i < 600; ++i)
        {
            nodes.push_back(cc::node_allocation<T24B_Align8>::create_from(alloc, u64(i * 5)));
        }

        for (int i = 0; i < 600; ++i)
        {
            CHECK(nodes[i].ptr->a == u64(i * 5));
        }
        // All freed at scope exit
    }

    SECTION("pattern B: 65B align 1 - interleaved 50 alive")
    {
        std::array<cc::node_allocation<T65B_Align1>, 50> active;

        for (int iter = 0; iter < 2000; ++iter)
        {
            int const idx = iter % 50;
            active[idx] = cc::node_allocation<T65B_Align1>::create_from(alloc, u8(iter));
        }

        for (int i = 0; i < 50; ++i)
        {
            int const expected = 1950 + i;
            CHECK(active[i].ptr->data[0] == u8(expected));
        }
    }

    SECTION("pattern C: 24B align 8 - alloc all, free in reverse")
    {
        cc::vector<cc::node_allocation<T24B_Align8>> nodes;
        for (int i = 0; i < 350; ++i)
        {
            nodes.push_back(cc::node_allocation<T24B_Align8>::create_from(alloc, u64(i * 11)));
        }

        // Free in reverse
        for (int i = 349; i >= 0; --i)
        {
            CHECK(nodes[i].ptr->a == u64(i * 11));
            nodes[i] = cc::node_allocation<T24B_Align8>{};
        }
    }

    SECTION("pattern C: 65B align 1 - alloc all, free alternating")
    {
        cc::vector<cc::node_allocation<T65B_Align1>> nodes;
        for (int i = 0; i < 200; ++i)
        {
            nodes.push_back(cc::node_allocation<T65B_Align1>::create_from(alloc, u8(i)));
        }

        // Free evens
        for (int i = 0; i < 200; i += 2)
        {
            nodes[i] = cc::node_allocation<T65B_Align1>{};
        }

        // Verify odds still valid
        for (int i = 1; i < 200; i += 2)
        {
            CHECK(nodes[i].is_valid());
            CHECK(nodes[i].ptr->data[0] == u8(i));
        }
    }
}

TEST("node_allocation - move-assignment from subobject safety")
{
    auto& alloc = cc::default_node_allocator();

    // Struct containing a node_allocation as a subobject
    struct Node
    {
        cc::node_allocation<Node> child;
        int id = 0;
    };

    SECTION("move-assign from subobject - correct ordering")
    {
        // outer owns a Node
        cc::node_allocation<Node> outer = cc::node_allocation<Node>::create_from(alloc);
        outer.ptr->id = 1;

        // outer.ptr->child owns another Node (same handle type!)
        outer.ptr->child = cc::node_allocation<Node>::create_from(alloc);
        outer.ptr->child.ptr->id = 2;

        // Poison pill: rhs is a subobject inside *outer.ptr
        // Wrong impl does reset() first => destroys outer.ptr => rhs becomes dangling => UB.
        // Correct impl does steal first, then reset => safe.
        outer = cc::move(outer.ptr->child);

        // Postconditions for correct impl:
        // - outer now owns the former child node (id == 2)
        CHECK(outer.ptr != nullptr);
        CHECK(outer.ptr->id == 2);
    }

    SECTION("move-assign from nested subobject")
    {
        // Create outer with child and grandchild
        cc::node_allocation<Node> outer = cc::node_allocation<Node>::create_from(alloc);
        outer.ptr->id = 1;

        outer.ptr->child = cc::node_allocation<Node>::create_from(alloc);
        outer.ptr->child.ptr->id = 2;

        outer.ptr->child.ptr->child = cc::node_allocation<Node>::create_from(alloc);
        outer.ptr->child.ptr->child.ptr->id = 3;

        // Move grandchild to outer - tests even deeper nesting
        outer = cc::move(outer.ptr->child.ptr->child);

        CHECK(outer.ptr != nullptr);
        CHECK(outer.ptr->id == 3);
    }

    SECTION("move-assign from subobject - multiple iterations")
    {
        // Test the pattern repeatedly to catch any state corruption
        for (int iter = 0; iter < 100; ++iter)
        {
            cc::node_allocation<Node> outer = cc::node_allocation<Node>::create_from(alloc);
            outer.ptr->id = iter * 10;

            outer.ptr->child = cc::node_allocation<Node>::create_from(alloc);
            outer.ptr->child.ptr->id = iter * 10 + 1;

            outer = cc::move(outer.ptr->child);

            CHECK(outer.ptr != nullptr);
            CHECK(outer.ptr->id == iter * 10 + 1);
        }
    }
}
