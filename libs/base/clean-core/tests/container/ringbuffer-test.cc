#include <clean-core/common/utility.hh>
#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// Tracks ctor/dtor balance so we can assert elements are constructed / destroyed exactly once.
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

/// A ring of capacity `cap` holding 0 .. count-1, with the front parked at slot `start`.
/// Rotating the content is the only way to reach the wrapped layouts from the public API.
cc::ringbuffer<int> make_rotated(isize cap, isize start, isize count)
{
    auto rb = cc::ringbuffer<int>::create_with_capacity(cap);
    for (isize i = 0; i < start; ++i)
        rb.push_back_stable(-1);
    rb.remove_front_n(start);
    for (isize i = 0; i < count; ++i)
        rb.push_back_stable(int(i));
    return rb;
}

bool holds_iota(cc::ringbuffer<int> const& rb, isize count)
{
    if (rb.size() != count)
        return false;
    for (isize i = 0; i < count; ++i)
        if (rb[i] != int(i))
            return false;
    return true;
}
} // namespace

TEST("ringbuffer - empty")
{
    cc::ringbuffer<int> rb;
    CHECK(rb.size() == 0);
    CHECK(rb.empty());
    CHECK(rb.capacity() == 0);
    CHECK(rb.free_capacity() == 0);
    CHECK(rb.full()); // 0 == capacity, and a push still works by growing
    CHECK(rb.begin() == rb.end());

    rb.push_back(7);
    CHECK(rb.size() == 1);
    CHECK(rb.front() == 7);
    CHECK(rb.back() == 7);
    CHECK(rb.capacity() == 16); // a cacheline's worth of ints
}

TEST("ringbuffer - fifo through a full wrap")
{
    auto rb = cc::ringbuffer<int>::create_with_capacity(4);
    CHECK(rb.capacity() == 4);

    // three laps around a four-slot ring, one push and one pop at a time
    for (int i = 0; i < 12; ++i)
    {
        rb.push_back_stable(i);
        CHECK(rb.size() == 1);
        CHECK(rb.front() == i);
        CHECK(rb.pop_front() == i);
    }
    CHECK(rb.empty());
    CHECK(rb.capacity() == 4); // _stable never grows
}

TEST("ringbuffer - both ends")
{
    cc::ringbuffer<int> rb;
    rb.push_back(2);
    rb.push_back(3);
    rb.push_front(1);
    rb.push_front(0);

    CHECK(holds_iota(rb, 4));
    CHECK(rb.front() == 0);
    CHECK(rb.back() == 3);

    CHECK(rb.pop_back() == 3);
    CHECK(rb.pop_front() == 0);
    CHECK(rb.size() == 2);
    CHECK(rb[0] == 1);
    CHECK(rb[1] == 2);
}

TEST("ringbuffer - initializer list and indexing")
{
    cc::ringbuffer<int> rb = {0, 1, 2, 3, 4};
    CHECK(holds_iota(rb, 5));
    CHECK(rb.capacity() == 8);

    rb.remove_front();
    CHECK(rb[0] == 1);
    CHECK(rb.size() == 4);
}

TEST("ringbuffer - capacity is always a power of two")
{
    cc::ringbuffer<int> rb;
    CHECK(rb.capacity() == 0);

    rb.reserve(5);
    CHECK(rb.capacity() == 8); // reserve is exact, no growth factor on top

    rb.reserve(3); // already covered
    CHECK(rb.capacity() == 8);

    for (int i = 0; i < 100; ++i)
    {
        rb.push_back(i);
        CHECK(cc::has_single_bit(u64(rb.capacity())));
        CHECK(rb.capacity() >= rb.size());
    }
    CHECK(rb.capacity() == 128);
}

TEST("ringbuffer - growth preserves order across a wrap")
{
    auto rb = make_rotated(4, 3, 4); // full, wrapped: slots 3,0,1,2
    CHECK(holds_iota(rb, 4));
    CHECK(rb.full());

    rb.push_back(4); // must grow
    CHECK(rb.capacity() == 8);
    CHECK(holds_iota(rb, 5));

    rb.push_front(-1);
    CHECK(rb.front() == -1);
    CHECK(rb[1] == 0);
    CHECK(rb.size() == 6);
}

TEST("ringbuffer - shrink_to_fit")
{
    auto rb = make_rotated(16, 13, 5);
    CHECK(rb.capacity() == 16);

    rb.shrink_to_fit();
    CHECK(rb.capacity() == 8);
    CHECK(holds_iota(rb, 5));

    rb.clear();
    rb.shrink_to_fit();
    CHECK(rb.capacity() == 0);
    CHECK(rb.empty());
}

TEST("ringbuffer - segments")
{
    auto rb = make_rotated(8, 6, 5); // slots 6,7,0,1,2

    auto const [a, b] = rb.segments();
    CHECK(a.size() == 2);
    CHECK(b.size() == 3);
    CHECK(a[0] == 0);
    CHECK(a[1] == 1);
    CHECK(b[0] == 2);
    CHECK(b[2] == 4);

    auto const linear = make_rotated(8, 0, 3);
    auto const [c, d] = linear.segments();
    CHECK(c.size() == 3);
    CHECK(d.empty());
}

TEST("ringbuffer - linearize, all three layouts")
{
    {
        auto rb = make_rotated(8, 2, 4); // linear already, sitting at slot 2
        auto const s = rb.linearize();
        CHECK(s.size() == 4);
        CHECK(s.data() == &rb[0]); // not moved to slot 0
        CHECK(holds_iota(rb, 4));
    }
    {
        auto rb = make_rotated(8, 6, 5); // wrapped, with a gap
        auto const s = rb.linearize();
        CHECK(s.size() == 5);
        CHECK(s.data() == &rb.front());
        CHECK(holds_iota(rb, 5));
        for (isize i = 0; i < 5; ++i)
            CHECK(s[i] == int(i));
    }
    {
        auto rb = make_rotated(4, 3, 4); // wrapped and exactly full: a rotation
        auto const s = rb.linearize();
        CHECK(s.size() == 4);
        CHECK(holds_iota(rb, 4));
        for (isize i = 0; i < 4; ++i)
            CHECK(s[i] == int(i));
    }
    {
        cc::ringbuffer<int> rb;
        CHECK(rb.linearize().empty());
    }
}

TEST("ringbuffer - overwriting keeps the newest elements")
{
    auto rb = cc::ringbuffer<int>::create_with_capacity(4);
    for (int i = 0; i < 10; ++i)
        rb.push_back_overwriting(i);

    CHECK(rb.size() == 4);
    CHECK(rb.capacity() == 4); // never grows
    CHECK(rb[0] == 6);
    CHECK(rb[3] == 9);

    for (int i = 0; i < 4; ++i)
        rb.push_front_overwriting(-i);
    CHECK(rb.front() == -3);
    CHECK(rb.back() == -0);
    CHECK(rb.size() == 4);
}

TEST("ringbuffer - try_push and try_pop")
{
    auto rb = cc::ringbuffer<int>::create_with_capacity(2);
    CHECK(rb.try_push_back(1));
    CHECK(rb.try_push_front(0));
    CHECK(!rb.try_push_back(2)); // full, and try_ never grows
    CHECK(!rb.try_push_front(2));
    CHECK(rb.size() == 2);
    CHECK(rb.capacity() == 2);

    CHECK(rb.try_pop_front().value() == 0);
    CHECK(rb.try_pop_back().value() == 1);
    CHECK(!rb.try_pop_front().has_value());
    CHECK(!rb.try_pop_back().has_value());
}

TEST("ringbuffer - ranges")
{
    auto const source = cc::vector<int>{2, 3, 4};

    auto rb = make_rotated(8, 6, 2); // wrapped, so the append splits into two runs
    rb.push_back_range(source);
    CHECK(holds_iota(rb, 5));

    rb.push_front_range(cc::span<int const>(source).first_n(2)); // keeps the source order
    CHECK(rb[0] == 2);
    CHECK(rb[1] == 3);
    CHECK(rb[2] == 0);
    CHECK(rb.size() == 7);

    int buffer[7] = {};
    rb.copy_to(buffer);
    CHECK(buffer[0] == 2);
    CHECK(buffer[2] == 0);
    CHECK(buffer[6] == 4);
}

TEST("ringbuffer - iterator is random access")
{
    auto rb = make_rotated(8, 5, 6);

    CHECK(rb.end() - rb.begin() == 6);
    CHECK(*(rb.begin() + 3) == 3);
    CHECK(*(3 + rb.begin()) == 3);
    CHECK(*(rb.end() - 1) == 5);
    CHECK(rb.begin()[2] == 2);
    CHECK(rb.begin() < rb.end());
    CHECK(!(rb.end() < rb.begin()));

    auto it = rb.begin();
    it += 4;
    CHECK(*it == 4);
    it -= 2;
    CHECK(*it == 2);
    CHECK(*it++ == 2);
    CHECK(*it-- == 3);
    CHECK(*it == 2);

    int sum = 0;
    for (auto const v : rb)
        sum += v;
    CHECK(sum == 0 + 1 + 2 + 3 + 4 + 5);
}

TEST("ringbuffer - value semantics")
{
    auto rb = make_rotated(8, 6, 5);

    auto copy = rb;
    CHECK(holds_iota(copy, 5));
    copy.push_back(99);
    CHECK(rb.size() == 5); // deep copy, untouched

    auto moved = cc::move(rb);
    CHECK(holds_iota(moved, 5));
    CHECK(rb.empty()); // NOLINT(bugprone-use-after-move) - a moved-from ring is empty
    CHECK(rb.capacity() == 0);

    cc::ringbuffer<int> assigned;
    assigned = moved;
    CHECK(holds_iota(assigned, 5));
    assigned = cc::move(moved);
    CHECK(holds_iota(assigned, 5));
}

TEST("ringbuffer - hash is independent of the internal rotation")
{
    auto const a = make_rotated(8, 0, 5);
    auto const b = make_rotated(8, 6, 5);
    auto const c = make_rotated(16, 11, 5);

    CHECK(hash(a) == hash(b));
    CHECK(hash(a) == hash(c));

    auto const d = make_rotated(8, 0, 4);
    CHECK(hash(a) != hash(d));
}

TEST("ringbuffer - element lifetimes")
{
    CHECK(Tracked::alive == 0);
    {
        auto rb = cc::ringbuffer<Tracked>::create_with_capacity(4);
        for (int i = 0; i < 4; ++i)
            rb.push_back_stable(Tracked(i));
        CHECK(Tracked::alive == 4);

        // pop two and push two: the wrap must not construct or destroy anything extra
        CHECK(rb.pop_front().value == 0);
        CHECK(rb.pop_front().value == 1);
        CHECK(Tracked::alive == 2);
        rb.push_back_stable(Tracked(4));
        rb.push_back_stable(Tracked(5));
        CHECK(Tracked::alive == 4);

        rb.push_back(Tracked(6)); // grows, moving the wrapped content over
        CHECK(Tracked::alive == 5);
        CHECK(rb.front().value == 2);
        CHECK(rb.back().value == 6);

        rb.linearize();
        CHECK(Tracked::alive == 5);

        auto const copy = rb;
        CHECK(Tracked::alive == 10);

        rb.remove_front_n(2);
        rb.remove_back_n(1);
        CHECK(Tracked::alive == 7);

        rb.clear();
        CHECK(Tracked::alive == 5);
    }
    CHECK(Tracked::alive == 0);
}

TEST("ringbuffer - move-only elements")
{
    cc::ringbuffer<MoveOnly> rb;
    for (int i = 0; i < 20; ++i) // enough to force a growth
        rb.emplace_back(i);

    CHECK(rb.size() == 20);
    CHECK(rb[0].value == 0);
    CHECK(rb[19].value == 19);

    auto const popped = rb.pop_front();
    CHECK(popped.value == 0);

    rb.emplace_front(-1);
    CHECK(rb.front().value == -1);

    auto moved = cc::move(rb);
    CHECK(moved.size() == 20);
}

TEST("ringbuffer - preconditions assert")
{
    cc::ringbuffer<int> rb;
    CHECK_ASSERTS(rb.pop_front());
    CHECK_ASSERTS(rb.pop_back());
    CHECK_ASSERTS(rb.front());
    CHECK_ASSERTS(rb.push_back_overwriting(1)); // no capacity to overwrite into

    rb.push_back(1);
    CHECK_ASSERTS(rb[1]);
    CHECK_ASSERTS(rb[-1]);
    CHECK_ASSERTS(rb.remove_front_n(2));

    auto full = cc::ringbuffer<int>::create_with_capacity(1);
    full.push_back_stable(0);
    CHECK_ASSERTS(full.push_back_stable(1));
    CHECK_ASSERTS(full.push_front_stable(1));
}
