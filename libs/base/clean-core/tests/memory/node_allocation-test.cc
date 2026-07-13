#include "node_allocation-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/node_allocation.hh>
#include <nexus/test.hh>

#include <array>

// Core handle semantics of cc::node_allocation<T>: construction, destruction, move, validity,
// alignment, and behavior across trivial / non-trivial / immovable payloads.
// Alloc/dealloc stress patterns live in node_allocation-patterns-test.cc, size-class edge cases in
// node_allocation-sizes-test.cc, slab lifecycle in node_allocation-slabs-test.cc, and the
// type-erased / polymorphic handles in node_allocation-handles-test.cc.

using namespace cc::primitive_defines;

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
