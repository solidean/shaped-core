#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_ringbuffer.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Packing: the two counters use the smallest unsigned type that fits N, and sit in front of the aligned slot storage.
static_assert(sizeof(cc::fixed_ringbuffer<u8, 10>) == 12, "u8/10 -> two u8 counters + 10 slots");
static_assert(sizeof(cc::fixed_ringbuffer<u8, 300>) == 304, "u8/300 -> two u16 counters, u8 cannot hold 300");
static_assert(sizeof(cc::fixed_ringbuffer<u64, 2>) == 24, "u64/2 -> two u8 counters padded up to the u64 alignment");

namespace
{
struct Tracked
{
    int value = 0;
    static inline int alive = 0;

    Tracked() { ++alive; }
    explicit Tracked(int v) : value(v) { ++alive; }
    Tracked(Tracked const& rhs) : value(rhs.value) { ++alive; }
    Tracked(Tracked&& rhs) noexcept : value(rhs.value) { ++alive; }
    Tracked& operator=(Tracked const&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
};

struct MoveOnly
{
    int value = 0;
    MoveOnly() = default;
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(MoveOnly&& rhs) noexcept : value(rhs.value) { rhs.value = -1; }
    MoveOnly& operator=(MoveOnly&& rhs) noexcept
    {
        value = rhs.value;
        rhs.value = -1;
        return *this;
    }
    MoveOnly(MoveOnly const&) = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;
};

/// A ring holding 0 .. count-1 with the front parked at slot `start`.
template <isize N>
cc::fixed_ringbuffer<int, N> make_rotated(isize start, isize count)
{
    cc::fixed_ringbuffer<int, N> rb;
    for (isize i = 0; i < start; ++i)
        rb.push_back(-1);
    rb.remove_front_n(start);
    for (isize i = 0; i < count; ++i)
        rb.push_back(int(i));
    return rb;
}

template <isize N>
bool holds_iota(cc::fixed_ringbuffer<int, N> const& rb, isize count)
{
    if (rb.size() != count)
        return false;
    for (isize i = 0; i < count; ++i)
        if (rb[i] != int(i))
            return false;
    return true;
}
} // namespace

TEST("fixed_ringbuffer - empty")
{
    cc::fixed_ringbuffer<int, 4> rb;
    CHECK(rb.size() == 0);
    CHECK(rb.empty());
    CHECK(!rb.full());
    CHECK(rb.capacity() == 4);
    CHECK((cc::fixed_ringbuffer<int, 4>::capacity() == 4));
    CHECK(rb.free_capacity() == 4);
    CHECK(rb.has_capacity_for(4));
    CHECK(!rb.has_capacity_for(5));
    CHECK(rb.begin() == rb.end());
}

TEST("fixed_ringbuffer - N == 0 is a valid permanently-empty ring")
{
    cc::fixed_ringbuffer<int, 0> rb;
    CHECK(rb.size() == 0);
    CHECK(rb.empty());
    CHECK(rb.full()); // 0 == capacity
    CHECK(rb.capacity() == 0);
    CHECK(rb.begin() == rb.end());
    CHECK(!rb.try_push_back(1));
    CHECK(!rb.try_pop_front().has_value());
    CHECK_ASSERTS(rb.push_back(1));

    auto const copy = rb; // copy / move of an N == 0 ring is well-formed
    CHECK(copy.empty());
}

TEST("fixed_ringbuffer - fifo through a full wrap")
{
    // an odd capacity: the wrap is a compare-and-subtract, so it does not need a power of two
    cc::fixed_ringbuffer<int, 3> rb;
    for (int i = 0; i < 10; ++i)
    {
        rb.push_back(i);
        CHECK(rb.back() == i);
        CHECK(rb.pop_front() == i);
    }
    CHECK(rb.empty());

    for (int i = 0; i < 3; ++i)
        rb.push_back(i);
    CHECK(rb.full());
    CHECK(holds_iota(rb, 3));
}

TEST("fixed_ringbuffer - both ends")
{
    auto rb = make_rotated<8>(6, 4); // wrapped: slots 6,7,0,1
    CHECK(holds_iota(rb, 4));

    rb.push_front(-1);
    CHECK(rb.front() == -1);
    CHECK(rb.pop_back() == 3);
    CHECK(rb.pop_front() == -1);
    CHECK(rb.size() == 3);
    CHECK(rb[0] == 0);
}

TEST("fixed_ringbuffer - overwriting keeps the newest samples")
{
    cc::fixed_ringbuffer<int, 4> rb;
    for (int i = 0; i < 100; ++i)
        rb.push_back_overwriting(i);

    CHECK(rb.size() == 4);
    CHECK(rb[0] == 96);
    CHECK(rb[3] == 99);

    cc::fixed_ringbuffer<int, 3> front;
    for (int i = 0; i < 5; ++i)
        front.push_front_overwriting(i);
    CHECK(front[0] == 4);
    CHECK(front[2] == 2);
}

TEST("fixed_ringbuffer - segments and linearize")
{
    {
        auto rb = make_rotated<8>(6, 5); // wrapped, with a gap
        auto const [a, b] = rb.segments();
        CHECK(a.size() == 2);
        CHECK(b.size() == 3);

        auto const s = rb.linearize();
        CHECK(s.size() == 5);
        CHECK(s.data() == &rb.front());
        for (isize i = 0; i < 5; ++i)
            CHECK(s[i] == int(i));
    }
    {
        auto rb = make_rotated<3>(2, 3); // wrapped and exactly full: a rotation
        auto const s = rb.linearize();
        CHECK(s.size() == 3);
        for (isize i = 0; i < 3; ++i)
            CHECK(s[i] == int(i));
        CHECK(holds_iota(rb, 3));
    }
}

TEST("fixed_ringbuffer - ranges and copy_to")
{
    int const source[] = {2, 3, 4};

    auto rb = make_rotated<8>(6, 2);
    rb.push_back_range(cc::span<int const>(source));
    CHECK(holds_iota(rb, 5));

    int out[5] = {};
    rb.copy_to(out);
    CHECK(out[0] == 0);
    CHECK(out[4] == 4);

    cc::fixed_ringbuffer<int, 4> small;
    small.push_back(9);
    small.push_front_range(cc::span<int const>(source));
    CHECK(small[0] == 2);
    CHECK(small[2] == 4);
    CHECK(small[3] == 9);
    CHECK(small.full());
}

TEST("fixed_ringbuffer - value semantics")
{
    auto const rb = make_rotated<8>(6, 5);

    auto copy = rb;
    CHECK(holds_iota(copy, 5));
    copy.push_back(99);
    CHECK(rb.size() == 5);

    auto moved = cc::move(copy);
    CHECK(moved.size() == 6);
    CHECK(copy.empty()); // NOLINT(bugprone-use-after-move) - a moved-from ring is empty

    cc::fixed_ringbuffer<int, 8> assigned;
    assigned = rb;
    CHECK(holds_iota(assigned, 5));
    assigned = cc::move(moved);
    CHECK(assigned.size() == 6);
}

TEST("fixed_ringbuffer - hash is independent of the internal rotation")
{
    auto const a = make_rotated<8>(0, 5);
    auto const b = make_rotated<8>(6, 5);
    CHECK(hash(a) == hash(b));
    CHECK(hash(a) != hash(make_rotated<8>(0, 4)));
}

TEST("fixed_ringbuffer - element lifetimes")
{
    CHECK(Tracked::alive == 0);
    {
        cc::fixed_ringbuffer<Tracked, 4> rb;
        for (int i = 0; i < 4; ++i)
            rb.push_back(Tracked(i));
        CHECK(Tracked::alive == 4);

        for (int i = 4; i < 20; ++i)
            rb.push_back_overwriting(Tracked(i)); // drops one for each it takes
        CHECK(Tracked::alive == 4);
        CHECK(rb.front().value == 16);

        rb.linearize();
        CHECK(Tracked::alive == 4);

        auto const copy = rb;
        CHECK(Tracked::alive == 8);

        rb.clear();
        CHECK(Tracked::alive == 4);
    }
    CHECK(Tracked::alive == 0);
}

TEST("fixed_ringbuffer - move-only elements")
{
    cc::fixed_ringbuffer<MoveOnly, 4> rb;
    for (int i = 0; i < 4; ++i)
        rb.emplace_back(i);

    CHECK(rb.front().value == 0);
    auto const popped = rb.pop_front();
    CHECK(popped.value == 0);

    rb.emplace_front(-1);
    CHECK(rb.front().value == -1);

    auto const moved = cc::move(rb);
    CHECK(moved.size() == 4);
}

TEST("fixed_ringbuffer - preconditions assert")
{
    cc::fixed_ringbuffer<int, 2> rb;
    CHECK_ASSERTS(rb.pop_front());
    CHECK_ASSERTS(rb.back());

    rb.push_back(0);
    rb.push_back(1);
    CHECK(rb.full());
    CHECK_ASSERTS(rb.push_back(2));
    CHECK_ASSERTS(rb.push_front(2));
    CHECK_ASSERTS(rb[2]);
    CHECK_ASSERTS(rb.remove_back_n(3));
}
