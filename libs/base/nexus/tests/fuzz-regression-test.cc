#include <clean-core/math/random.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>

TEST("fuzz regression - add1 reaches 7")
{
    auto t = nx::fuzz::test::create();
    t->add_value("3", 3);
    t->add_op("add1", [](int a) { return a + 1; });
    t->add_invariant("is-not-7", [](int i) { return i != 7; });

    SECTION("emitted eval_op chain reproduces the failure")
    {
        // mirrors what the emitter prints: rebuild the values via eval_op, chaining results
        auto i0 = t->eval_op("3");
        auto i1 = t->eval_op("add1", i0);
        auto i2 = t->eval_op("add1", i1);
        auto i3 = t->eval_op("add1", i2);
        auto i4 = t->eval_op("add1", i3);

        CHECK(i4.get<int>() == 7);
        CHECK(!t->eval_op_bool("is-not-7", i4)); // the invariant is false at 7 -> reproduces the finding
    }

    SECTION("emit_regression renders a usable reproducer")
    {
        nx::fuzz::test::fuzz_result res;
        for (int s = 1; s <= 64; ++s)
        {
            res = t->execute_fuzzer(s);
            if (!res.is_ok && res.failing_run.has_value())
                break;
        }
        REQUIRE(res.failing_run.has_value());

        cc::random rng{1u};
        auto minimized = res.failing_run.value().minimize(rng);
        auto code = minimized.emit_regression("test", nx::fuzz::nexus_section_dialect());

        CHECK(code.contains("eval_op"));
        CHECK(code.contains("add1"));
        CHECK(code.contains("is-not-7"));
        CHECK(code.contains("eval_op_bool"));
    }
}

TEST("fuzz regression - from_state reproduces a recorded operation")
{
    auto t = nx::fuzz::test::create();
    t->add_op("gen", [](cc::random& r) { return r.uniform(0, 1000000); });

    // same recorded state -> same drawn value, every time (the blessed replay roundtrip)
    int const a = t->eval_op_to<int>("gen", cc::random::from_state(4242));
    int const b = t->eval_op_to<int>("gen", cc::random::from_state(4242));
    CHECK(a == b);
}
