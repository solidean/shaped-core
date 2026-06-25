#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

TEST("random - same seed yields same sequence")
{
    cc::random a(12345);
    cc::random b(12345);

    SECTION("u32 stream matches")
    {
        for (int i = 0; i < 64; ++i)
            CHECK(a.next_u32() == b.next_u32());
    }

    SECTION("u64 stream matches")
    {
        for (int i = 0; i < 64; ++i)
            CHECK(a.next_u64() == b.next_u64());
    }
}

TEST("random - different seeds diverge")
{
    cc::random a(1);
    cc::random b(2);

    // not a hard guarantee, but the first draw must not collide for these seeds
    CHECK(a.next_u64() != b.next_u64());
}

TEST("random - seed() resets the stream")
{
    cc::random r(7);
    u64 const first = r.next_u64();
    (void)r.next_u64();
    (void)r.next_u64();

    r.seed(7);
    CHECK(r.next_u64() == first);
}

TEST("random - clone is independent but identical")
{
    cc::random r(99);
    (void)r.next_u32(); // advance so we are not at the seed origin

    auto c = r.clone();
    for (int i = 0; i < 32; ++i)
        CHECK(r.next_u32() == c.next_u32());
}

TEST("random - uniform integer stays in range")
{
    cc::random r(2024);

    SECTION("positive range")
    {
        for (int i = 0; i < 10000; ++i)
        {
            int const v = r.uniform(3, 9);
            CHECK(v >= 3);
            CHECK(v <= 9);
        }
    }

    SECTION("signed range crossing zero")
    {
        for (int i = 0; i < 10000; ++i)
        {
            int const v = r.uniform(-5, 5);
            CHECK(v >= -5);
            CHECK(v <= 5);
        }
    }

    SECTION("degenerate single-value range")
    {
        for (int i = 0; i < 100; ++i)
            CHECK(r.uniform(42, 42) == 42);
    }
}

TEST("random - uniform integer covers the whole range")
{
    cc::random r(555);
    bool seen[7] = {};
    for (int i = 0; i < 100000; ++i)
        seen[r.uniform(0, 6)] = true;

    for (bool s : seen)
        CHECK(s);
}

TEST("random - uniform float stays in range")
{
    cc::random r(31415);

    SECTION("float")
    {
        for (int i = 0; i < 10000; ++i)
        {
            f32 const v = r.uniform(1.0f, 2.0f);
            CHECK(v >= 1.0f);
            CHECK(v < 2.0f);
        }
    }

    SECTION("double")
    {
        for (int i = 0; i < 10000; ++i)
        {
            f64 const v = r.uniform(-1.0, 1.0);
            CHECK(v >= -1.0);
            CHECK(v < 1.0);
        }
    }
}

TEST("random - shuffle is a reproducible permutation")
{
    auto make = []
    {
        cc::vector<int> v;
        for (int i = 0; i < 50; ++i)
            v.push_back(i);
        return v;
    };

    cc::random r1(808);
    cc::random r2(808);

    auto a = make();
    auto b = make();
    r1.shuffle(a);
    r2.shuffle(b);

    SECTION("same seed shuffles identically")
    {
        REQUIRE(a.size() == b.size());
        for (cc::isize i = 0; i < a.size(); ++i)
            CHECK(a[i] == b[i]);
    }

    SECTION("still a permutation of the input")
    {
        bool seen[50] = {};
        for (int v : a)
        {
            REQUIRE(v >= 0);
            REQUIRE(v < 50);
            CHECK(!seen[v]); // no duplicates
            seen[v] = true;
        }
    }
}

TEST("random - uniform_in picks a valid element")
{
    cc::random r(2);
    cc::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);

    for (int i = 0; i < 1000; ++i)
    {
        int const picked = r.uniform_in(v);
        CHECK((picked == 10 || picked == 20 || picked == 30));
    }
}
