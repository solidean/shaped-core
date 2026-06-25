#include <clean-core/math/random.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>

#include <stdexcept>

namespace
{
// Finds the first seed that produces a failing run, or leaves the result un-set.
nx::fuzz::test::fuzz_result find_failing(nx::fuzz::test& t, int max_seed)
{
    for (int s = 1; s <= max_seed; ++s)
    {
        auto res = t.execute_fuzzer(s);
        if (!res.is_ok && res.failing_run.has_value())
            return res;
    }
    return nx::fuzz::test::fuzz_result{};
}
} // namespace

TEST("fuzz minimize - add1/is-not-7")
{
    auto t = nx::fuzz::test::create();
    t->add_value("3", 3);
    t->add_op("add1", [](int a) { return a + 1; });
    t->add_invariant("is-not-7", [](int i) { return i != 7; });

    auto res = find_failing(*t, 64);
    REQUIRE(res.failing_run.has_value());

    SECTION("shrinks to 6 operations")
    {
        cc::random rng{1u};
        auto minimized = res.failing_run.value().minimize(rng);

        // value "3" + 4x add1 (3->4->5->6->7) + the failing is-not-7 check == 6
        CHECK(int(minimized.operations.size()) == 6);
    }

    SECTION("minimized run still reproduces the failure")
    {
        cc::random rng{2u};
        auto minimized = res.failing_run.value().minimize(rng);

        auto replay = minimized.replay();
        CHECK(replay.is_failing());
    }
}

TEST("fuzz minimize - seeded gen/test shrinks to 3 operations")
{
    auto t = nx::fuzz::test::create();
    t->add_op("gen", [](cc::random& r) { return r.uniform(0, 3); });
    t->add_op("test",
              [](int a, int b)
              {
                  if (a == 2 && b == 0)
                      throw std::runtime_error("bad combination");
              });

    auto res = find_failing(*t, 256);
    REQUIRE(res.failing_run.has_value());

    cc::random rng{7u};
    auto minimized = res.failing_run.value().minimize(rng);

    // two distinct gens feeding the failing test call
    CHECK(int(minimized.operations.size()) == 3);
}
