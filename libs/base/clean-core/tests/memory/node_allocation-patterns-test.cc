#include "node_allocation-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <array>

// Alloc/dealloc throughput patterns across the size classes:
//   pattern A - allocate all, then free all
//   pattern B - interleaved alloc/dealloc with a bounded live set
//   pattern C - allocate all, free in reverse / forward / alternating order
// plus multi-slab growth and mixed-size-class churn.

using namespace cc::primitive_defines;

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
