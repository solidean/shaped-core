#include <nexus/test.hh>
#include <shaped-graphics/command_list_slot.hh>

// Pure unit tests for the command-list slot allocator: lowest-clear-bit allocation, the "returns to zero"
// release signal, and the overflow path past 64 concurrent slots.

TEST("sg slot - lowest free slot, returns-to-zero on last release")
{
    sg::command_list_slot_allocator alloc;

    auto const a = alloc.acquire();
    auto const b = alloc.acquire();
    CHECK(int(a) == 0);
    CHECK(int(b) == 1);
    CHECK(alloc.live_count() == 2);

    CHECK(alloc.release(a) == false); // b still live
    CHECK(alloc.live_count() == 1);
    CHECK(alloc.release(b) == true); // none left -> the promote signal
    CHECK(alloc.live_count() == 0);
}

TEST("sg slot - reuses the lowest freed index")
{
    sg::command_list_slot_allocator alloc;
    auto const s0 = alloc.acquire();
    auto const s1 = alloc.acquire();
    auto const s2 = alloc.acquire();
    CHECK(int(s2) == 2);

    (void)alloc.release(s1); // free the middle slot
    auto const s1b = alloc.acquire();
    CHECK(int(s1b) == 1); // lowest clear bit is reused

    (void)alloc.release(s0);
    (void)alloc.release(s1b);
    CHECK(alloc.release(s2) == true);
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

    (void)alloc.release(overflow);
    for (int i = 0; i < 64; ++i)
    {
        bool const last = alloc.release(slots[i]);
        CHECK(last == (i == 63)); // only the final release returns to zero
    }
}
