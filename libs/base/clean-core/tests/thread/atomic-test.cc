#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// cc::atomic_add / _sub / _and / _or / _xor: read-modify-write on a plain lvalue, returning the old value.
// Single-threaded by construction, so these pin the return-old-value contract against whichever
// cc::atomic_ref the build has — std::atomic_ref, or the no-threads shim.

TEST("atomic - atomic_add performs atomic addition")
{
    SECTION("u32 addition")
    {
        u32 counter = 0;
        u32 old_val = cc::atomic_add(counter, u32(5));
        CHECK(old_val == 0);
        CHECK(counter == 5);

        old_val = cc::atomic_add(counter, u32(3));
        CHECK(old_val == 5);
        CHECK(counter == 8);
    }

    SECTION("u64 addition")
    {
        u64 counter = 100;
        u64 old_val = cc::atomic_add(counter, u64(25));
        CHECK(old_val == 100);
        CHECK(counter == 125);
    }

    SECTION("i32 addition")
    {
        i32 counter = 10;
        i32 old_val = cc::atomic_add(counter, i32(-5));
        CHECK(old_val == 10);
        CHECK(counter == 5);
    }

    SECTION("sequential additions")
    {
        u32 value = 0;
        for (u32 i = 0; i < 10; ++i)
        {
            cc::atomic_add(value, u32(1));
        }
        CHECK(value == 10);
    }
}

TEST("atomic - atomic_sub performs atomic subtraction")
{
    SECTION("u32 subtraction")
    {
        u32 counter = 10;
        u32 old_val = cc::atomic_sub(counter, u32(3));
        CHECK(old_val == 10);
        CHECK(counter == 7);

        old_val = cc::atomic_sub(counter, u32(2));
        CHECK(old_val == 7);
        CHECK(counter == 5);
    }

    SECTION("u64 subtraction")
    {
        u64 counter = 1000;
        u64 old_val = cc::atomic_sub(counter, u64(250));
        CHECK(old_val == 1000);
        CHECK(counter == 750);
    }

    SECTION("i32 subtraction")
    {
        i32 counter = 5;
        i32 old_val = cc::atomic_sub(counter, i32(-10));
        CHECK(old_val == 5);
        CHECK(counter == 15);
    }

    SECTION("sequential subtractions")
    {
        u32 value = 20;
        for (u32 i = 0; i < 5; ++i)
        {
            cc::atomic_sub(value, u32(2));
        }
        CHECK(value == 10);
    }
}

TEST("atomic - atomic_and performs atomic bitwise AND")
{
    SECTION("u32 AND operations")
    {
        u32 flags = 0b1111;
        u32 old_val = cc::atomic_and(flags, u32(0b1100));
        CHECK(old_val == 0b1111);
        CHECK(flags == 0b1100);

        old_val = cc::atomic_and(flags, u32(0b1010));
        CHECK(old_val == 0b1100);
        CHECK(flags == 0b1000);
    }

    SECTION("u64 AND operations")
    {
        u64 flags = 0xFFFFFFFFFFFFFFFFull;
        u64 old_val = cc::atomic_and(flags, u64(0x00000000FFFFFFFFull));
        CHECK(old_val == 0xFFFFFFFFFFFFFFFFull);
        CHECK(flags == 0x00000000FFFFFFFFull);
    }

    SECTION("clearing specific bits")
    {
        u32 value = 0b11111111;
        cc::atomic_and(value, u32(0b11110000)); // clear lower 4 bits
        CHECK(value == 0b11110000);
    }

    SECTION("sequential AND operations")
    {
        u32 value = 0xFF;
        cc::atomic_and(value, u32(0xF0));
        cc::atomic_and(value, u32(0xC0));
        cc::atomic_and(value, u32(0x80));
        CHECK(value == 0x80);
    }
}

TEST("atomic - atomic_or performs atomic bitwise OR")
{
    SECTION("u32 OR operations")
    {
        u32 flags = 0b0011;
        u32 old_val = cc::atomic_or(flags, u32(0b1100));
        CHECK(old_val == 0b0011);
        CHECK(flags == 0b1111);

        flags = 0b0000;
        old_val = cc::atomic_or(flags, u32(0b0101));
        CHECK(old_val == 0b0000);
        CHECK(flags == 0b0101);
    }

    SECTION("u64 OR operations")
    {
        u64 flags = 0x00000000FFFFFFFFull;
        u64 old_val = cc::atomic_or(flags, u64(0xFFFFFFFF00000000ull));
        CHECK(old_val == 0x00000000FFFFFFFFull);
        CHECK(flags == 0xFFFFFFFFFFFFFFFFull);
    }

    SECTION("setting specific bits")
    {
        u32 value = 0b00000000;
        cc::atomic_or(value, u32(0b00001111)); // set lower 4 bits
        CHECK(value == 0b00001111);
    }

    SECTION("sequential OR operations")
    {
        u32 value = 0x00;
        cc::atomic_or(value, u32(0x01));
        cc::atomic_or(value, u32(0x02));
        cc::atomic_or(value, u32(0x04));
        cc::atomic_or(value, u32(0x08));
        CHECK(value == 0x0F);
    }
}

TEST("atomic - atomic_xor performs atomic bitwise XOR")
{
    SECTION("u32 XOR operations")
    {
        u32 flags = 0b0011;
        u32 old_val = cc::atomic_xor(flags, u32(0b1100));
        CHECK(old_val == 0b0011);
        CHECK(flags == 0b1111);

        old_val = cc::atomic_xor(flags, u32(0b1111));
        CHECK(old_val == 0b1111);
        CHECK(flags == 0b0000);
    }

    SECTION("u64 XOR operations")
    {
        u64 flags = 0xAAAAAAAAAAAAAAAAull;
        u64 old_val = cc::atomic_xor(flags, u64(0x5555555555555555ull));
        CHECK(old_val == 0xAAAAAAAAAAAAAAAAull);
        CHECK(flags == 0xFFFFFFFFFFFFFFFFull);
    }

    SECTION("toggling bits")
    {
        u32 value = 0b10101010;
        cc::atomic_xor(value, u32(0b11111111)); // flip all bits
        CHECK(value == 0b01010101);
        cc::atomic_xor(value, u32(0b11111111)); // flip back
        CHECK(value == 0b10101010);
    }

    SECTION("XOR with zero is identity")
    {
        u32 value = 0xDEADBEEF;
        cc::atomic_xor(value, u32(0));
        CHECK(value == 0xDEADBEEF);
    }

    SECTION("XOR with self gives zero")
    {
        u32 value = 0xDEADBEEF;
        u32 original = value;
        cc::atomic_xor(value, original);
        CHECK(value == 0);
    }
}

TEST("atomic - atomic operations return old values")
{
    SECTION("return values are pre-operation values")
    {
        u32 value = 10;

        u32 old = cc::atomic_add(value, u32(5));
        CHECK(old == 10);   // old value before add
        CHECK(value == 15); // new value after add

        old = cc::atomic_sub(value, u32(3));
        CHECK(old == 15);   // old value before sub
        CHECK(value == 12); // new value after sub

        old = cc::atomic_and(value, u32(0x0F));
        CHECK(old == 12);   // old value before and
        CHECK(value == 12); // new value after and (12 & 15 = 12)

        old = cc::atomic_or(value, u32(0x10));
        CHECK(old == 12);   // old value before or
        CHECK(value == 28); // new value after or (12 | 16 = 28)

        old = cc::atomic_xor(value, u32(0xFF));
        CHECK(old == 28);    // old value before xor
        CHECK(value == 227); // new value after xor (28 ^ 255 = 227)
    }
}
