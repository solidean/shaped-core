#pragma once

#include <clean-core/memory/node_allocation.hh>

using namespace cc::primitive_defines;

// Shared test types for the node_allocation test suite, split across several files.
// Kept in an anonymous namespace: internal linkage per TU, so each file gets its own copy of the
// static counters (TrackedDtor / NonTrivialType) — every test is self-contained within one TU.
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

// Over-aligned large nodes (> 256 B payload, alignment > 8): these take the large path AND require the
// returned pointer to honor an alignment greater than the header's 8 bytes.
struct alignas(32) T512B_Align32
{
    u8 data[512] = {};
    explicit T512B_Align32(u8 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(sizeof(T512B_Align32) == 512);
static_assert(alignof(T512B_Align32) == 32);

struct alignas(64) T300B_Align64
{
    u8 data[300] = {};
    explicit T300B_Align64(u8 v = 0)
    {
        for (auto& d : data)
            d = v;
    }
};
static_assert(alignof(T300B_Align64) == 64);
static_assert(sizeof(T300B_Align64) > 256); // large path
} // namespace
