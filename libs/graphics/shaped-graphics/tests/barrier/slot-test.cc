#include <nexus/test.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>

// Pure unit tests for the command-list slot allocator.
// Lowest-clear-bit allocation, the live count, and the overflow path past 64 concurrent slots.

TEST("sg slot - lowest free slot, and the live count follows")
{
    sg::command_list_slot_allocator alloc;

    auto const a = alloc.acquire();
    auto const b = alloc.acquire();
    CHECK(int(a) == 0);
    CHECK(int(b) == 1);
    CHECK(alloc.live_count() == 2);

    alloc.release(a);
    CHECK(alloc.live_count() == 1); // b still live
    alloc.release(b);
    CHECK(alloc.live_count() == 0);
}

TEST("sg slot - reuses the lowest freed index")
{
    sg::command_list_slot_allocator alloc;
    auto const s0 = alloc.acquire();
    auto const s1 = alloc.acquire();
    auto const s2 = alloc.acquire();
    CHECK(int(s2) == 2);

    alloc.release(s1); // free the middle slot
    auto const s1b = alloc.acquire();
    CHECK(int(s1b) == 1); // lowest clear bit is reused

    alloc.release(s0);
    alloc.release(s1b);
    alloc.release(s2);
    CHECK(alloc.live_count() == 0);
}

TEST("sg slot - overflow past 64 concurrent slots")
{
    sg::command_list_slot_allocator alloc;

    cc::vector<sg::command_list_slot> slots;
    for (int i = 0; i < 64; ++i)
        slots.push_back(alloc.acquire());
    CHECK(alloc.live_count() == 64);

    // The 65th allocation overflows to index 64 (emits a one-time warning to stderr).
    auto const overflow = alloc.acquire();
    CHECK(int(overflow) == 64);
    CHECK(alloc.live_count() == 65);

    alloc.release(overflow);
    for (int i = 0; i < 64; ++i)
        alloc.release(slots[i]);
    CHECK(alloc.live_count() == 0);
}
