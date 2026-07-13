#include "node_allocation-test-types.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <array>

// Size-class and alignment edge cases: structs that don't land on a power-of-two boundary (rounded up
// to the next class), the large path (> 256 B), and over-aligned large nodes whose payload must honor
// an alignment stronger than the 8-byte header.

using namespace cc::primitive_defines;

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

TEST("node_allocation - over-aligned large nodes honor alignment")
{
    auto& alloc = cc::default_node_allocator();

    // > 256 B with alignment > 8 takes the large path; the returned payload must honor the alignment.
    // Regression: the old header layout returned payload = alloc_ptr + 24, which is not 32-aligned.
    cc::vector<cc::node_allocation<T512B_Align32>> live;
    for (int i = 0; i < 40; ++i)
    {
        auto n = cc::node_allocation<T512B_Align32>::create_from(alloc, u8(i));
        CHECK(cc::is_aligned(n.ptr, 32));
        CHECK(n.ptr->data[0] == u8(i));
        CHECK(n.ptr->data[511] == u8(i));
        live.push_back(cc::move(n));
    }
    // all still valid and aligned while co-resident, then freed (dtor round-trips the aligned header)
    for (cc::isize i = 0; i < live.size(); ++i)
    {
        CHECK(cc::is_aligned(live[i].ptr, 32));
        CHECK(live[i].ptr->data[7] == u8(i));
    }

    // a stronger alignment allocated alongside
    auto b = cc::node_allocation<T300B_Align64>::create_from(alloc, u8(0xAB));
    CHECK(cc::is_aligned(b.ptr, 64));
    CHECK(b.ptr->data[299] == u8(0xAB));
}
